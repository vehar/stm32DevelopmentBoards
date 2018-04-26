/*******************************************************************************
 * Test unit for the development board: STM32F405RGT6
 * *****************************************************************************
 * Copyright (C) 2016-2017 Mikhail Kulesh
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 ******************************************************************************/

#include "StmPlusPlus/StmPlusPlus.h"
#include "StmPlusPlus/WavStreamer.h"
#include "EspSender.h"

#include <array>

using namespace StmPlusPlus;
using namespace StmPlusPlus::Devices;

#define USART_DEBUG_MODULE "Main: "

class MyApplication : public RealTimeClock::EventHandler, WavStreamer::EventHandler
{
public:

    static const size_t INPUT_PINS = 8;  // Number of monitored input pins

private:
    
    UsartLogger log;

    RealTimeClock rtc;
    IOPin ledGreen, ledBlue, ledRed;
    PeriodicalEvent hardBitEvent;
    IOPin mco;

    // Interrupt priorities
    InterruptPriority irqPrioI2S;
    InterruptPriority irqPrioEsp;
    InterruptPriority irqPrioSd;
    InterruptPriority irqPrioRtc;

    // SD card
    IOPin pinSdPower, pinSdDetect;
    IOPort portSd1, portSd2;
    SdCard sdCard;
    bool sdCardInserted;

    // Configuration
    Config config;

    // ESP
    Esp11 esp;
    EspSender espSender;

    // Input pins
    std::array<IOPin, INPUT_PINS> pins;
    std::array<bool, INPUT_PINS> pinsState;

    // Message
    char messageBuffer[2048];

    // I2S2 Audio
    I2S i2s;
    AudioDac_UDA1334 audioDac;
    WavStreamer streamer;
    IOPin playButton;

    // NTP
    static const size_t NTP_PACKET_SIZE = 48;  // NTP time is in the first 48 bytes of message
    struct NtpPacket {
            uint8_t flags;
            uint8_t stratum;
            uint8_t poll;
            uint8_t precision;
            uint32_t root_delay;
            uint32_t root_dispersion;
            uint8_t referenceID[4];
            uint32_t ref_ts_sec;
            uint32_t ref_ts_frac;
            uint32_t origin_ts_sec;
            uint32_t origin_ts_frac;
            uint32_t recv_ts_sec;
            uint32_t recv_ts_frac;
            uint32_t trans_ts_sec;
            uint32_t trans_ts_frac;
    } __attribute__((__packed__));

    struct NtpPacket ntpPacket;

public:
    
    MyApplication () :
            // logging
            log(Usart::USART_1, IOPort::B, GPIO_PIN_6, GPIO_PIN_7, 115200),
            
            // RTC
            rtc(),
            ledGreen(IOPort::C, GPIO_PIN_1, GPIO_MODE_OUTPUT_PP),
            ledBlue(IOPort::C, GPIO_PIN_2, GPIO_MODE_OUTPUT_PP),
            ledRed(IOPort::C, GPIO_PIN_3, GPIO_MODE_OUTPUT_PP),
            hardBitEvent(rtc, 10, 2),
            mco(IOPort::A, GPIO_PIN_8, GPIO_MODE_AF_PP),
            
            // Interrupt priorities
            irqPrioI2S(6, 0), // I2S DMA interrupt priority: 7 will be also used
            irqPrioEsp(5, 0),
            irqPrioSd(3, 0), // SD DMA interrupt priority: 4 will be also used
            irqPrioRtc(2, 0),
            
            // SD card
            pinSdPower(IOPort::A, GPIO_PIN_15, GPIO_MODE_OUTPUT_PP, GPIO_PULLDOWN, GPIO_SPEED_HIGH, true, false),
            pinSdDetect(IOPort::B, GPIO_PIN_3, GPIO_MODE_INPUT, GPIO_PULLUP),
            portSd1(IOPort::C,
                    /* mode     = */GPIO_MODE_OUTPUT_PP,
                    /* pull     = */GPIO_PULLUP,
                    /* speed    = */GPIO_SPEED_FREQ_VERY_HIGH,
                    /* pin      = */GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12,
                    /* callInit = */false),
            portSd2(IOPort::D,
                    /* mode     = */GPIO_MODE_OUTPUT_PP,
                    /* pull     = */GPIO_PULLUP,
                    /* speed    = */GPIO_SPEED_FREQ_VERY_HIGH,
                    /* pin      = */GPIO_PIN_2,
                    /* callInit = */false),
            sdCard(pinSdDetect, portSd1, portSd2),
            sdCardInserted(false),
            
            // Configuration
            config(pinSdPower, sdCard, "conf.txt"),

            //ESP
            esp(Usart::USART_2, IOPort::A, GPIO_PIN_2, GPIO_PIN_3, irqPrioEsp, IOPort::A, GPIO_PIN_1, Timer::TIM_6),
            espSender(esp, ledRed),

            // Input pins
            pins { { IOPin(IOPort::A, GPIO_PIN_4,  GPIO_MODE_INPUT, GPIO_PULLDOWN),
                     IOPin(IOPort::A, GPIO_PIN_5,  GPIO_MODE_INPUT, GPIO_PULLDOWN),
                     IOPin(IOPort::A, GPIO_PIN_6,  GPIO_MODE_INPUT, GPIO_PULLDOWN),
                     IOPin(IOPort::A, GPIO_PIN_7,  GPIO_MODE_INPUT, GPIO_PULLDOWN),
                     IOPin(IOPort::C, GPIO_PIN_4,  GPIO_MODE_INPUT, GPIO_PULLDOWN),
                     IOPin(IOPort::C, GPIO_PIN_5,  GPIO_MODE_INPUT, GPIO_PULLDOWN),
                     IOPin(IOPort::B, GPIO_PIN_0,  GPIO_MODE_INPUT, GPIO_PULLDOWN),
                     IOPin(IOPort::B, GPIO_PIN_1,  GPIO_MODE_INPUT, GPIO_PULLDOWN)
            } },

            // I2S2 Audio Configuration
            // PB10 --> I2S2_CK
            // PB12 --> I2S2_WS
            // PB15 --> I2S2_SD
            i2s(IOPort::B, GPIO_PIN_10 | GPIO_PIN_12 | GPIO_PIN_15, irqPrioI2S),
            audioDac(i2s,
                     /* power    = */ IOPort::B, GPIO_PIN_11,
                     /* mute     = */ IOPort::B, GPIO_PIN_13,
                     /* smplFreq = */ IOPort::B, GPIO_PIN_14),
            streamer(sdCard, audioDac),
            playButton(IOPort::B, GPIO_PIN_2, GPIO_MODE_INPUT, GPIO_PULLUP, GPIO_SPEED_LOW)

    {
        mco.activateClockOutput(RCC_MCO1SOURCE_PLLCLK, RCC_MCODIV_4);
    }
    
    virtual ~MyApplication ()
    {
        // empty
    }
    
    inline RealTimeClock & getRtc ()
    {
        return rtc;
    }
    
    inline I2S & getI2S ()
    {
        return i2s;
    }

    inline Esp11 & getEsp ()
    {
        return esp;
    }

    void run ()
    {
        log.initInstance();
        HAL_Delay(100);

        USART_DEBUG("--------------------------------------------------------");
        USART_DEBUG("Oscillator frequency: " 
                << System::getExternalOscillatorFreq() << ", MCU frequency: " << System::getMcuFreq());
        
        HAL_StatusTypeDef status = HAL_TIMEOUT;
        do
        {
            status = rtc.start(8 * 2047 + 7, RTC_WAKEUPCLOCK_RTCCLK_DIV2, irqPrioRtc, this);
            USART_DEBUG("RTC start status: " << status);
        }
        while (status != HAL_OK);

        sdCard.setIrqPrio(irqPrioSd);
        sdCard.initInstance();
        if (sdCard.isCardInserted())
        {
            updateSdCardState();
        }
        
        USART_DEBUG("Input pins: " << pins.size());
        pinsState.fill(false);
        USART_DEBUG("Pin state: " << fillMessage());
        esp.assignSendLed(&ledGreen);

        streamer.stop();
        streamer.setHandler(this);
        streamer.setVolume(0.5);

        bool reportState = false;
        espSender.sendMessage(config, "UDP", "192.168.1.1", "123", getNTPrequst(), NTP_PACKET_SIZE);
        while (true)
        {
            updateSdCardState();
            if (!playButton.getBit())
            {
                if (streamer.isActive())
                {
                    USART_DEBUG("Stopping WAV");
                    streamer.stop();
                    HAL_Delay(500);
                }
                else
                {
                    USART_DEBUG("Starting WAV");
                    streamer.start(AudioDac_UDA1334::SourceType:: STREAM, "S44.WAV");
                    HAL_Delay(500);
                }
            }
            streamer.periodic();
            if (isInputPinsChanged())
            {
                USART_DEBUG("Input pins change detected");
                ledBlue.putBit(true);
                reportState = true;
            }
            if (reportState && espSender.isOutputMessageSent())
            {
                espSender.sendMessage(config, "TCP", config.getServerIp(), config.getServerPort(), fillMessage());
                reportState = false;
            }
            espSender.periodic(rtc.getTimeSec());
            if (!reportState && espSender.isOutputMessageSent())
            {
                ledBlue.putBit(false);
            }
            if (hardBitEvent.isOccured())
            {
                ledGreen.putBit(hardBitEvent.occurance() == 1);
            }
            if (esp.isListening() && esp.getInputMessageSize() > 0)
            {
                USART_DEBUG("Received " << esp.getInputMessageSize() << " bytes");
                esp.getInputMessage(messageBuffer, esp.getInputMessageSize());
                decodeNtpMessage();
            }
        }
    }
    
    bool isInputPinsChanged ()
    {
        bool isChanged = false;
        for (size_t i = 0; i < INPUT_PINS; ++i)
        {
            if (pins[i].getBit() != pinsState[i])
            {
                isChanged = true;
                pinsState[i] = pins[i].getBit();
            }
        }
        return isChanged;
    }
    
    virtual void onRtcWakeUp ()
    {
        if (espSender.isOutputMessageSent() && rtc.getTimeSec() % 2 == 0)
        {
            hardBitEvent.resetTime();
        }
    }
    
    void updateSdCardState ()
    {
        if (!sdCardInserted && sdCard.isCardInserted())
        {
            config.readConfiguration();
        }
        sdCardInserted = sdCard.isCardInserted();
    }

    const char * fillMessage ()
    {
        char digits[9];
        ::__utoa(rtc.getTimeSec(), digits, 10);
        ::strcpy(messageBuffer, "<message>");
        ::strcat(messageBuffer, "<name>BOARD_STATE</name>");
        // the first parameter: board ID
        ::strcat(messageBuffer, "<p>");
        ::strcat(messageBuffer, config.getBoardId());
        ::strcat(messageBuffer, "</p>");
        // the second parameter: state
        ::strcat(messageBuffer, "<p>");
        for (auto &p : pins)
        {
            ::__itoa(p.getBit(), digits, 10);
            ::strcat(messageBuffer, digits);
        }
        ::strcat(messageBuffer, "</p>");
        ::strcat(messageBuffer, "</message>");
        return &messageBuffer[0];
    }

    inline void processDmaTxCpltCallback (I2S_HandleTypeDef * /*channel*/)
    {
        audioDac.onBlockTransmissionFinished();
    }

    virtual bool onStartSteaming (Devices::AudioDac_UDA1334::SourceType s)
    {
        if (s == Devices::AudioDac_UDA1334::SourceType::STREAM)
        {
            if (!sdCard.isCardInserted())
            {
                USART_DEBUG("SD Card is not inserted");
                return false;
            }
            sdCard.clearPort();
            pinSdPower.setLow();
            HAL_Delay(250);
        }
        return true;
    }

    virtual void onFinishSteaming ()
    {
        pinSdPower.setHigh();
    }

    // NTP
    #define UNIX_OFFSET             2208988800L
    #define ENDIAN_SWAP32(data)     ((data >> 24) | /* right shift 3 bytes */ \
                                    ((data & 0x00ff0000) >> 8) | /* right shift 1 byte */ \
                                    ((data & 0x0000ff00) << 8) | /* left shift 1 byte */ \
                                    ((data & 0x000000ff) << 24)) /* left shift 3 bytes */

    const char * getNTPrequst ()
    {
        ::memset(&ntpPacket, 0, NTP_PACKET_SIZE);
        ntpPacket.flags = 0xe3;
        return (const char *) &ntpPacket;
    }

    time_t decodeNtpMessage ()
    {
        ::memcpy(&ntpPacket, messageBuffer, NTP_PACKET_SIZE);
        ntpPacket.root_delay = ENDIAN_SWAP32(ntpPacket.root_delay);
        ntpPacket.root_dispersion = ENDIAN_SWAP32(ntpPacket.root_dispersion);
        ntpPacket.ref_ts_sec = ENDIAN_SWAP32(ntpPacket.ref_ts_sec);
        ntpPacket.ref_ts_frac = ENDIAN_SWAP32(ntpPacket.ref_ts_frac);
        ntpPacket.origin_ts_sec = ENDIAN_SWAP32(ntpPacket.origin_ts_sec);
        ntpPacket.origin_ts_frac = ENDIAN_SWAP32(ntpPacket.origin_ts_frac);
        ntpPacket.recv_ts_sec = ENDIAN_SWAP32(ntpPacket.recv_ts_sec);
        ntpPacket.recv_ts_frac = ENDIAN_SWAP32(ntpPacket.recv_ts_frac);
        ntpPacket.trans_ts_sec = ENDIAN_SWAP32(ntpPacket.trans_ts_sec);
        ntpPacket.trans_ts_frac = ENDIAN_SWAP32(ntpPacket.trans_ts_frac);

        unsigned int recv_secs = ntpPacket.recv_ts_sec - UNIX_OFFSET; /* convert to unix time */
        time_t total_secs = recv_secs;
        USART_DEBUG("Unix time: " << (unsigned int)total_secs);
        struct tm * now = ::gmtime(&total_secs);

        char logStr[48];
        sprintf(logStr, "%02d/%02d/%d %02d:%02d:%02d", now->tm_mday, now->tm_mon+1, now->tm_year+1900, now->tm_hour, now->tm_min, now->tm_sec);
        USART_DEBUG("Now: " << logStr);
        return total_secs;
    }

};

MyApplication * appPtr = NULL;

int main (void)
{
    // Note: check the Value of the External oscillator mounted in PCB
    // and set this value in the file stm32f4xx_hal_conf.h
    
    HAL_Init();
    
    IOPort defaultPortA(IOPort::PortName::A, GPIO_MODE_INPUT, GPIO_PULLDOWN);
    IOPort defaultPortB(IOPort::PortName::B, GPIO_MODE_INPUT, GPIO_PULLDOWN);
    IOPort defaultPortC(IOPort::PortName::C, GPIO_MODE_INPUT, GPIO_PULLDOWN);
    
    // Set system frequency to 168MHz
    System::ClockDiv clkDiv;
    clkDiv.PLLM = 16;
    clkDiv.PLLN = 336;
    clkDiv.PLLP = 2;
    clkDiv.PLLQ = 7;
    clkDiv.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clkDiv.APB1CLKDivider = RCC_HCLK_DIV8;
    clkDiv.APB2CLKDivider = RCC_HCLK_DIV8;
    clkDiv.PLLI2SN = 192;
    clkDiv.PLLI2SR = 2;
    do
    {
        System::setClock(clkDiv, FLASH_LATENCY_3, System::RtcType::RTC_EXT);
    }
    while (System::getMcuFreq() != 168000000L);
    
    MyApplication app;
    appPtr = &app;
    
    app.run();
}

extern "C"
{
void SysTick_Handler (void)
{
    HAL_IncTick();
    appPtr->getRtc().onMilliSecondInterrupt();
}

void TIM5_IRQHandler ()
{
    // empty
}

void RTC_WKUP_IRQHandler ()
{
    appPtr->getRtc().onSecondInterrupt();
}

void DMA2_Stream3_IRQHandler (void)
{
    Devices::SdCard::getInstance()->processDmaRxInterrupt();
}

void DMA2_Stream6_IRQHandler (void)
{
    Devices::SdCard::getInstance()->processDmaTxInterrupt();
}

void SDIO_IRQHandler (void)
{
    Devices::SdCard::getInstance()->processSdIOInterrupt();
}

void USART2_IRQHandler (void)
{
    appPtr->getEsp().processInterrupt();
}

void HAL_UART_TxCpltCallback (UART_HandleTypeDef * channel)
{
    if (channel->Instance == USART2)
    {
        appPtr->getEsp().processTxCpltCallback();
    }
}

void HAL_UART_RxCpltCallback (UART_HandleTypeDef * channel)
{
    if (channel->Instance == USART2)
    {
        appPtr->getEsp().processRxCpltCallback();
    }
}

void HAL_UART_ErrorCallback (UART_HandleTypeDef * channel)
{
    if (channel->Instance == USART2)
    {
        appPtr->getEsp().processErrorCallback();
    }
}

void SPI2_IRQHandler(void)
{
    appPtr->getI2S().processI2SInterrupt();
}

void DMA1_Stream4_IRQHandler(void)
{
    appPtr->getI2S().processDmaTxInterrupt();
}

void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef *channel)
{
    appPtr->processDmaTxCpltCallback(channel);
}

}

