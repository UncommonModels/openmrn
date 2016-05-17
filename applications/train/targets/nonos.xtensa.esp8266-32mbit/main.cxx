/** \copyright
 * Copyright (c) 2013, Balazs Racz
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are  permitted provided that the following conditions are met:
 *
 *  - Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * \file main.cxx
 *
 * An application that blinks an LED.
 *
 * @author Balazs Racz
 * @date 3 Aug 2013
 */

#include <stdio.h>
#include <unistd.h>

#include "dcc/Defs.hxx"
#include "executor/StateFlow.hxx"
#include "nmranet/EventHandlerTemplates.hxx"
#include "nmranet/SimpleStack.hxx"
#include "nmranet/TractionTrain.hxx"
#include "nmranet/TrainInterface.hxx"
#include "os/os.h"
#include "utils/ESPWifiClient.hxx"
#include "utils/GpioInitializer.hxx"
#include "utils/blinker.h"
#include "freertos_drivers/common/BlinkerGPIO.hxx"
#include "freertos_drivers/common/DummyGPIO.hxx"
#include "freertos_drivers/esp8266/TimerBasedPwm.hxx"
#include "freertos_drivers/esp8266/Esp8266Gpio.hxx"

extern "C" {
#include <gpio.h>
#include <osapi.h>
#include <user_interface.h>
extern void ets_delay_us(uint32_t us);
#define os_delay_us ets_delay_us
}

#include "nmranet/TrainInterface.hxx"

struct HW
{
/* original / standard definitions.
    GPIO_PIN(MOT_A_HI, GpioOutputSafeLow, 4);
    GPIO_PIN(MOT_A_LO, GpioOutputSafeLow, 5);

    GPIO_PIN(MOT_B_HI, GpioOutputSafeLow, 14);
    GPIO_PIN(MOT_B_LO, GpioOutputSafeLow, 12);

    // forward: A=HI B=LO

    GPIO_PIN(LIGHT_FRONT, GpioOutputSafeLow, 13);
    GPIO_PIN(LIGHT_BACK, GpioOutputSafeLow, 15);

    GPIO_PIN(F1, GpioOutputSafeLow, 2);

    //typedef DummyPin F1_Pin;

*/

    GPIO_PIN(MOT_A_HI, GpioOutputSafeLow, 4);
    GPIO_PIN(MOT_A_LO, GpioOutputSafeLow, 5);

    GPIO_PIN(MOT_B_HI, GpioOutputSafeLow, 14);
    GPIO_PIN(MOT_B_LO, GpioOutputSafeLow, 12);

    // forward: A=HI B=LO

    //typedef BLINKER_Pin LIGHT_FRONT_Pin;
    GPIO_PIN(LIGHT_FRONT, GpioOutputSafeLow, 13);
    GPIO_PIN(LIGHT_BACK, GpioOutputSafeLow, 15);

    typedef DummyPin F1_Pin;

    typedef GpioInitializer<        //
        MOT_A_HI_Pin, MOT_A_LO_Pin, //
        MOT_B_HI_Pin, MOT_B_LO_Pin, //
        LIGHT_FRONT_Pin, LIGHT_BACK_Pin,
        F1_Pin> GpioInit;
};

struct SpeedRequest
{
    SpeedRequest()
    {
        reset();
    }
    nmranet::SpeedType speed_;
    bool emergencyStop_;
    void reset()
    {
        speed_ = 0.0;
        emergencyStop_ = false;
    }
};

class SpeedController : public StateFlow<Buffer<SpeedRequest>, QList<2>>
{
public:
    SpeedController(Service *s)
        : StateFlow<Buffer<SpeedRequest>, QList<2>>(s)
    {
        HW::MOT_A_HI_Pin::set_off();
        HW::MOT_B_HI_Pin::set_off();
        pwm_.enable();
    }

    void call_speed(nmranet::Velocity speed)
    {
        /*
        auto *b = alloc();
        b->data()->speed_ = speed;
        send(b, 1);*/
        long long period = USEC_TO_NSEC(1000);
        long long fill = speed_to_fill_rate(speed, period);
        //pwm_.old_set_state(2, period, period - fill);
        if (speed.direction() == speed.FORWARD) {
            GPIO_OUTPUT_SET(2, 1);
            pwm_.old_set_state(0, period, period - fill);
        } else {
            GPIO_OUTPUT_SET(0, 1);
            pwm_.old_set_state(2, period, period - fill);
        }
    }

    void call_estop()
    {
        auto *b = alloc();
        b->data()->emergencyStop_ = true;
        send(b, 0);
    }

private:
    long long speed_to_fill_rate(nmranet::SpeedType speed, long long period) {
        int fill_rate = speed.mph();
        if (fill_rate >= 128)
            fill_rate = 127;
        // Let's do a 1khz
        long long fill = (period * fill_rate) >> 7;
        return fill;
    }

    Action entry() override
    {
        if (req()->emergencyStop_)
        {
            pwm_.pause();
            HW::MOT_A_HI_Pin::set_off();
            HW::MOT_B_HI_Pin::set_off();
            release();
            return sleep_and_call(
                &timer_, MSEC_TO_NSEC(1), STATE(eoff_enablelow));
        }
        // Check if we need to change the direction.
        bool desired_dir =
            (req()->speed_.direction() == nmranet::SpeedType::FORWARD);
        if (lastDirMotAHi_ != desired_dir)
        {
            pwm_.pause();
            HW::MOT_B_HI_Pin::set_off();
            HW::MOT_A_HI_Pin::set_off();
            return sleep_and_call(&timer_, MSEC_TO_NSEC(1), STATE(do_speed));
        }
        return call_immediately(STATE(do_speed));
    }

    Action do_speed()
    {
        // We set the pins explicitly for safety
        bool desired_dir =
            (req()->speed_.direction() == nmranet::SpeedType::FORWARD);
        int lo_pin;
        if (desired_dir)
        {
            HW::MOT_B_HI_Pin::set_off();
            HW::MOT_A_LO_Pin::set_off();
            lo_pin = HW::MOT_B_LO_Pin::PIN;
            HW::MOT_A_HI_Pin::set_on();
        }
        else
        {
            HW::MOT_A_HI_Pin::set_off();
            HW::MOT_B_LO_Pin::set_off();
            lo_pin = HW::MOT_A_LO_Pin::PIN;
            HW::MOT_B_HI_Pin::set_on();
        }

        long long period = USEC_TO_NSEC(1000);
        pwm_.set_state(lo_pin, speed_to_fill_rate(req()->speed_, period), period);
        lastDirMotAHi_ = desired_dir;
        return release_and_exit();
    }

    Action eoff_enablelow()
    {
        // By shorting both motor outputs to ground we turn it to actively
        // brake.
        HW::MOT_A_LO_Pin::set_on();
        HW::MOT_B_LO_Pin::set_on();
        return exit();
    }

    SpeedRequest *req()
    {
        return message()->data();
    }

    TimerBasedPwm pwm_;
    StateFlowTimer timer_{this};
    bool lastDirMotAHi_{false};
};

extern SpeedController g_speed_controller;

class ESPHuzzahTrain : public nmranet::TrainImpl
{
public:
    ESPHuzzahTrain()
    {
        HW::GpioInit::hw_init();
        HW::LIGHT_FRONT_Pin::set(false);
        HW::LIGHT_BACK_Pin::set(false);
    }

    void set_speed(nmranet::SpeedType speed) override
    {
        lastSpeed_ = speed;
        g_speed_controller.call_speed(speed);
        if (f0)
        {
            if (speed.direction() == nmranet::SpeedType::FORWARD)
            {
                HW::LIGHT_FRONT_Pin::set(true);
                HW::LIGHT_BACK_Pin::set(false);
            }
            else
            {
                HW::LIGHT_BACK_Pin::set(true);
                HW::LIGHT_FRONT_Pin::set(false);
            }
        }
    }
    /** Returns the last set speed of the locomotive. */
    nmranet::SpeedType get_speed() override
    {
        return lastSpeed_;
    }

    /** Sets the train to emergency stop. */
    void set_emergencystop() override
    {
        //g_speed_controller.call_estop();
        lastSpeed_.set_mph(0); // keeps direction
    }

    /** Sets the value of a function.
     * @param address is a 24-bit address of the function to set. For legacy DCC
     * locomotives, see @ref TractionDefs for the address definitions (0=light,
     * 1-28= traditional function buttons).
     * @param value is the function value. For binary functions, any non-zero
     * value sets the function to on, zero sets it to off.*/
    void set_fn(uint32_t address, uint16_t value) override
    {
        switch (address)
        {
            case 0:
                f0 = value;
                if (!value)
                {
                    HW::LIGHT_FRONT_Pin::set(false);
                    HW::LIGHT_BACK_Pin::set(false);
                }
                else if (lastSpeed_.direction() == nmranet::SpeedType::FORWARD)
                {
                    HW::LIGHT_FRONT_Pin::set(true);
                    HW::LIGHT_BACK_Pin::set(false);
                }
                else
                {
                    HW::LIGHT_BACK_Pin::set(true);
                    HW::LIGHT_FRONT_Pin::set(false);
                }
                break;
            case 1:
                /*if (value) {
                    analogWrite(2, 700);
                } else {
                    analogWrite(2, 100);
                    }*/
                f1 = value;
                HW::F1_Pin::set(value);
                break;
        }
    }

    /** @returns the value of a function. */
    uint16_t get_fn(uint32_t address) override
    {
        switch (address)
        {
            case 0:
                return f0 ? 1 : 0;
                break;
            case 1:
                return f1 ? 1 : 0;
                break;
        }
        return 0;
    }

    uint32_t legacy_address() override
    {
        return 883;
    }

    /** @returns the type of legacy protocol in use. */
    dcc::TrainAddressType legacy_address_type() override
    {
        return dcc::TrainAddressType::DCC_LONG_ADDRESS;
    }

private:
    nmranet::SpeedType lastSpeed_ = 0.0;
    bool f0 = false;
    bool f1 = false;
};

namespace nmranet
{

class TrainSnipHandler : public IncomingMessageStateFlow
{
public:
    TrainSnipHandler(If *parent, SimpleInfoFlow *info_flow)
        : IncomingMessageStateFlow(parent)
        , responseFlow_(info_flow)
    {
        iface()->dispatcher()->register_handler(this,
            nmranet::Defs::MTI_IDENT_INFO_REQUEST, nmranet::Defs::MTI_EXACT);
    }
    ~TrainSnipHandler()
    {
        iface()->dispatcher()->unregister_handler(this,
            nmranet::Defs::MTI_IDENT_INFO_REQUEST, nmranet::Defs::MTI_EXACT);
    }

    Action entry() OVERRIDE
    {
        return allocate_and_call(responseFlow_, STATE(send_response_request));
    }

    Action send_response_request()
    {
        auto *b = get_allocation_result(responseFlow_);
        b->data()->reset(
            nmsg(), snipResponse_, nmranet::Defs::MTI_IDENT_INFO_REPLY);
        b->set_done(n_.reset(this));
        responseFlow_->send(b);
        release();
        return wait_and_call(STATE(send_done));
    }

    Action send_done()
    {
        return exit();
    }

private:
    SimpleInfoFlow *responseFlow_;
    BarrierNotifiable n_;
    static SimpleInfoDescriptor snipResponse_[];
};

nmranet::SimpleInfoDescriptor TrainSnipHandler::snipResponse_[] = {
    {nmranet::SimpleInfoDescriptor::LITERAL_BYTE, 4, 0, nullptr},
    {nmranet::SimpleInfoDescriptor::C_STRING, 41, 0, "Balazs Racz"},
    {nmranet::SimpleInfoDescriptor::C_STRING, 41, 0, "Dead-rail train"},
    {nmranet::SimpleInfoDescriptor::C_STRING, 21, 0, "ESP12"},
    {nmranet::SimpleInfoDescriptor::C_STRING, 21, 0, "0.1"},
    {nmranet::SimpleInfoDescriptor::LITERAL_BYTE, 2, 0, nullptr},
    {nmranet::SimpleInfoDescriptor::C_STRING, 63, 1, "E12 883"},
    {nmranet::SimpleInfoDescriptor::C_STRING, 64, 0, "No description"},
    {nmranet::SimpleInfoDescriptor::END_OF_DATA, 0, 0, 0}};

const char kFdiXml[] =
    R"(<?xml version='1.0' encoding='UTF-8'?>
<?xml-stylesheet type='text/xsl' href='xslt/fdi.xsl'?>
<fdi xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance' xsi:noNamespaceSchemaLocation='http://openlcb.org/trunk/prototypes/xml/schema/fdi.xsd'>
<segment space='249'><group><name/>
<function size='1' kind='binary'>
<name>Light</name>
<number>0</number>
</function>
<function size='1' kind='binary'>
<name>BlueL</name>
<number>1</number>
</function>
</group></segment></fdi>)";

struct DeadrailStack
{
    DeadrailStack()
    {
        AddAliasAllocator(trainNode_.node_id(), &ifCan_);
        memoryConfigHandler_.registry()->insert(
            &trainNode_, MemoryConfigDefs::SPACE_FDI, &fdiBlock_);
    }

    void start_stack()
    {
        ifCan_.alias_allocator()->send(ifCan_.alias_allocator()->alloc());
    }

    Executor<5> executor_{NO_THREAD()};
    Service service_{&executor_};
    CanHubFlow canHub0_{&service_};
    IfCan ifCan_{&executor_, &canHub0_, config_local_alias_cache_size(),
        config_remote_alias_cache_size(), config_local_nodes_count()};
    InitializeFlow initFlow_{&service_};
    EventService eventService_{&ifCan_};

    TrainService tractionService_{&ifCan_};
    ESPHuzzahTrain trainImpl_;
    TrainNode trainNode_{&tractionService_, &trainImpl_};
    FixedEventProducer<nmranet::TractionDefs::IS_TRAIN_EVENT>
        isTrainEventHandler{&trainNode_};

    SimpleInfoFlow infoFlow_{&ifCan_};
    TrainSnipHandler snipHandler_{&ifCan_, &infoFlow_};

    ProtocolIdentificationHandler pip{
        &trainNode_, nmranet::Defs::SIMPLE_PROTOCOL_SUBSET |
            nmranet::Defs::DATAGRAM | nmranet::Defs::MEMORY_CONFIGURATION |
            nmranet::Defs::EVENT_EXCHANGE |
            nmranet::Defs::SIMPLE_NODE_INFORMATION |
            nmranet::Defs::TRACTION_CONTROL | nmranet::Defs::TRACTION_FDI};

    CanDatagramService datagramService_{&ifCan_,
        config_num_datagram_registry_entries(), config_num_datagram_clients()};
    MemoryConfigHandler memoryConfigHandler_{
        &datagramService_, nullptr, config_num_memory_spaces()};

    ReadOnlyMemoryBlock fdiBlock_{
        reinterpret_cast<const uint8_t *>(kFdiXml), strlen(kFdiXml)};
};

extern Pool *const g_incoming_datagram_allocator = init_main_buffer_pool();

} // namespace nmranet

nmranet::DeadrailStack stack;

SpeedController g_speed_controller(&stack.service_);

extern "C" {
extern char WIFI_SSID[];
extern char WIFI_PASS[];
extern char WIFI_HUB_HOSTNAME[];
extern int WIFI_HUB_PORT;
extern void timer1_isr_init();
}


class TestBlinker : public StateFlowBase {
public:
    TestBlinker() : StateFlowBase(&stack.service_) {
        start_flow(STATE(doo));
        pwm_.enable();
    }

private:
    Action doo() {
        if (isOn_) {
            isOn_ = false;
            pwm_.old_set_state(2, USEC_TO_NSEC(1000), USEC_TO_NSEC(800));
            //analogWrite(2, 800);
        } else {
            isOn_ = true;
            pwm_.old_set_state(2, USEC_TO_NSEC(1000), USEC_TO_NSEC(200));
            //analogWrite(2, 200);
        }
        return sleep_and_call(&timer_, MSEC_TO_NSEC(500), STATE(doo));
    }

    TimerBasedPwm pwm_;
    StateFlowTimer timer_{this};
    bool isOn_{true};
};


/** Entry point to application.
 * @param argc number of command line arguments
 * @param argv array of command line arguments
 * @return 0, should never return
 */
int appl_main(int argc, char *argv[])
{
    new ESPWifiClient(WIFI_SSID, WIFI_PASS, &stack.canHub0_, WIFI_HUB_HOSTNAME,
        WIFI_HUB_PORT, []()
        {
            stack.executor_.thread_body();
            stack.start_stack();
        });
    //timer1_isr_init();
    return 0;
}
