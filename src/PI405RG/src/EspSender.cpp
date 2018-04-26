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

#include <EspSender.h>
#include <ctime>

using namespace StmPlusPlus;
using namespace Devices;

#define USART_DEBUG_MODULE "ESPS: "

/************************************************************************
 * Class EspSender
 ************************************************************************/

EspSender::EspSender (Devices::Esp11 & _esp, IOPin & _errorLed) :
        esp(_esp),
        errorLed(_errorLed),
        espState(Esp11::AsyncCmd::OFF),
        outputMessage(NULL),
        currentTime(0),
        repeatDelay(0),
        nextOperationTime(0),
        turnOffDelay(0),
        turnOffTime(__LONG_MAX__),
        asyncStates { {
            AsyncState(Esp11::AsyncCmd::POWER_ON,       Esp11::AsyncCmd::ECHO_OFF,       "power on"),
            AsyncState(Esp11::AsyncCmd::ECHO_OFF,       Esp11::AsyncCmd::ENSURE_READY,   "echo off"),
            AsyncState(Esp11::AsyncCmd::ENSURE_READY,   Esp11::AsyncCmd::SET_MODE,       "ensure ready"),
            AsyncState(Esp11::AsyncCmd::SET_MODE,       Esp11::AsyncCmd::ENSURE_MODE,    "set ESP mode"),
            AsyncState(Esp11::AsyncCmd::ENSURE_MODE,    Esp11::AsyncCmd::SET_ADDR,       "ensure set ESP"),
            AsyncState(Esp11::AsyncCmd::SET_ADDR,       Esp11::AsyncCmd::ENSURE_WLAN,    "set IP"),
            AsyncState(Esp11::AsyncCmd::ENSURE_WLAN,    Esp11::AsyncCmd::CONNECT_WLAN,   "search SSID"),
            AsyncState(Esp11::AsyncCmd::CONNECT_WLAN,   Esp11::AsyncCmd::PING_SERVER,    "connect SSID"),
            AsyncState(Esp11::AsyncCmd::PING_SERVER,    Esp11::AsyncCmd::SET_CON_MODE,   "ping server"),
            AsyncState(Esp11::AsyncCmd::SET_CON_MODE,   Esp11::AsyncCmd::SET_SINDLE_CON, "set normal connection mode"),
            AsyncState(Esp11::AsyncCmd::SET_SINDLE_CON, Esp11::AsyncCmd::CONNECT_SERVER, "set single connection"),
            AsyncState(Esp11::AsyncCmd::CONNECT_SERVER, Esp11::AsyncCmd::SEND_MSG_SIZE,  "connect server"),
            AsyncState(Esp11::AsyncCmd::SEND_MSG_SIZE,  Esp11::AsyncCmd::SEND_MESSAGE,   Esp11::AsyncCmd::RECONNECT, "send message size"),
            AsyncState(Esp11::AsyncCmd::SEND_MESSAGE,   Esp11::AsyncCmd::WAITING,        Esp11::AsyncCmd::RECONNECT, "send message"),
            AsyncState(Esp11::AsyncCmd::RECONNECT,      Esp11::AsyncCmd::PING_SERVER,    Esp11::AsyncCmd::PING_SERVER, "reconnect server"),
            AsyncState(Esp11::AsyncCmd::WAITING,        Esp11::AsyncCmd::WAITING,        "waiting message"),
            AsyncState(Esp11::AsyncCmd::DISCONNECT,     Esp11::AsyncCmd::POWER_OFF,      Esp11::AsyncCmd::POWER_OFF, "disconnect"),
            AsyncState(Esp11::AsyncCmd::POWER_OFF,      Esp11::AsyncCmd::OFF,            "power off"),
            AsyncState(Esp11::AsyncCmd::OFF,            Esp11::AsyncCmd::OFF,            "turned off")
        } }
{
    // empty
}

void EspSender::sendMessage (const Config & config, const char* protocol, const char * server, const char * port, const char * msg, size_t messageSize)
{
    esp.setMode(1);
    esp.setIp(config.getThisIp());
    esp.setGatway(config.getGateIp());
    esp.setMask(config.getIpMask());
    esp.setSsid(config.getWlanName());
    esp.setPasswd(config.getWlanPass());
    esp.setProtocol(protocol);
    esp.setServer(server);
    esp.setPort(port);
    turnOffDelay = config.getTurnOffDelay();
    repeatDelay = config.getRepeatDelay();
    outputMessage = msg;
    esp.setMessage(outputMessage);
    if (messageSize == 0)
    {
        messageSize = ::strlen(outputMessage);
    }
    esp.setMessageSize(messageSize);
    USART_DEBUG("Sending message to " << server << "/" << port << "[" << messageSize << "]: " << msg);
    if (espState == Esp11::AsyncCmd::OFF)
    {
        espState = Esp11::AsyncCmd::POWER_ON;
    }
    else if (espState == Esp11::AsyncCmd::WAITING)
    {
        esp.stopListening();
        espState = Esp11::AsyncCmd::SEND_MSG_SIZE;
    }
}

void EspSender::periodic (time_t seconds)
{
    if (espState == Esp11::AsyncCmd::OFF)
    {
        return;
    }

    currentTime = seconds;
    if (currentTime < nextOperationTime)
    {
        return;
    }

    if (espState == Esp11::AsyncCmd::WAITING)
    {
        if (outputMessage == NULL && currentTime > turnOffTime)
        {
            esp.stopListening();
            espState = Esp11::AsyncCmd::DISCONNECT;
        }
        else if (esp.isListening())
        {
            esp.periodic();
            if (esp.getInputMessageSize() > 0)
            {
                turnOffTime = currentTime + turnOffDelay;
            }
        }
        return;
    }
    
    if (!esp.isTransmissionStarted())
    {
        errorLed.putBit(false);
        const AsyncState * s = findState(espState);
        if (s == NULL)
        {
            USART_DEBUG("ESP state: state corrupted");
            return;
        }

        if (s->cmd == Esp11::AsyncCmd::POWER_ON)
        {
            turnOffTime = __LONG_MAX__;
        }
        if (s->cmd == Esp11::AsyncCmd::SEND_MSG_SIZE)
        {
            if (outputMessage == NULL)
            {
                return;
            }
        }

        if (!esp.transmit(s->cmd))
        {
            USART_DEBUG("ESP state: " << s->description << " -> failed to start transmission");
        }
    }
    else if (esp.isResponceAvailable())
    {
        const AsyncState * s = findState(espState);
        if (s == NULL)
        {
            USART_DEBUG("ESP state: state corrupted");
            return;
        }
        if (esp.getResponce(s->cmd))
        {
            espState = s->okNextCmd;
            stateReport(true, s->description);
            if (s->cmd == Esp11::AsyncCmd::SEND_MESSAGE)
            {
                outputMessage = NULL;
                turnOffTime = currentTime + turnOffDelay;
                esp.startListening();
            }
        }
        else
        {
            espState = s->errorNextCmd;
            delayNextOperation(repeatDelay);
            stateReport(false, s->description);
        }
        if (espState == Esp11::AsyncCmd::POWER_OFF)
        {
            delayNextOperation(repeatDelay);
        }
    }
    else
    {
        esp.periodic();
    }
}

void EspSender::stateReport (bool result, const char * description)
{
    if (result)
    {
        USART_DEBUG("ESP state: " << description << " -> OK");
        errorLed.putBit(false);
    }
    else
    {
        USART_DEBUG("ESP error: " << description << " -> ERROR");
        errorLed.putBit(true);
    }
}

const EspSender::AsyncState * EspSender::findState (Devices::Esp11::AsyncCmd st)
{
    for (auto & ss : asyncStates)
    {
        if (ss.cmd == st)
        {
            return &ss;
        }
    }
    return NULL;
}
