#include <cstdint>
#include <cstring>
#include "telemetry.h"

#if defined(UNIT_TEST)
#include <iostream>
using namespace std;
#endif

#if CRSF_RX_MODULE

#include "CRSF.h"
#include "devWIFI.h"

extern CRSF crsf;

Telemetry::Telemetry()
{
    ResetState();
}

bool Telemetry::ShouldCallBootloader()
{
    bool bootloader = callBootloader;
    callBootloader = false;
    return bootloader;
}

bool Telemetry::ShouldCallEnterBind()
{
    bool enterBind = callEnterBind;
    callEnterBind = false;
    return enterBind;
}

bool Telemetry::ShouldCallUpdateModelMatch()
{
    bool updateModelMatch = callUpdateModelMatch;
    callUpdateModelMatch = false;
    return updateModelMatch;
}

bool Telemetry::ShouldSendDeviceFrame()
{
    bool deviceFrame = sendDeviceFrame;
    sendDeviceFrame = false;
    return deviceFrame;
}


PAYLOAD_DATA(GPS, BATTERY_SENSOR, ATTITUDE, DEVICE_INFO, FLIGHT_MODE, VARIO);

bool Telemetry::GetNextPayload(uint8_t* nextPayloadSize, uint8_t **payloadData)
{
    uint8_t checks = 0;
    uint8_t oldPayloadIndex = currentPayloadIndex;
    uint8_t realLength = 0;

    if (payloadTypes[currentPayloadIndex].locked)
    {
        payloadTypes[currentPayloadIndex].locked = false;
        payloadTypes[currentPayloadIndex].updated = false;
    }

    do
    {
        currentPayloadIndex = (currentPayloadIndex + 1) % payloadTypesCount;
        checks++;
    } while(!payloadTypes[currentPayloadIndex].updated && checks < payloadTypesCount);

    if (payloadTypes[currentPayloadIndex].updated)
    {
        payloadTypes[currentPayloadIndex].locked = true;

        realLength = CRSF_FRAME_SIZE(payloadTypes[currentPayloadIndex].data[CRSF_TELEMETRY_LENGTH_INDEX]);
        // search for non zero data from the end
        while (realLength > 0 && payloadTypes[currentPayloadIndex].data[realLength - 1] == 0)
        {
            realLength--;
        }

        if (realLength > 0)
        {
            // store real length in frame
            payloadTypes[currentPayloadIndex].data[CRSF_TELEMETRY_LENGTH_INDEX] = realLength - CRSF_FRAME_NOT_COUNTED_BYTES;
            *nextPayloadSize = realLength;
            *payloadData = payloadTypes[currentPayloadIndex].data;
            return true;
        }
    }

    currentPayloadIndex = oldPayloadIndex;
    *nextPayloadSize = 0;
    *payloadData = 0;
    return false;
}

uint8_t Telemetry::UpdatedPayloadCount()
{
    uint8_t count = 0;
    for (int8_t i = 0; i < payloadTypesCount; i++)
    {
        if (payloadTypes[i].updated)
        {
            count++;
        }
    }

    return count;
}

uint8_t Telemetry::ReceivedPackagesCount()
{
    return receivedPackages;
}

void Telemetry::ResetState()
{
    telemetry_state = TELEMETRY_IDLE;
    currentTelemetryByte = 0;
    currentPayloadIndex = 0;
    receivedPackages = 0;

    uint8_t offset = 0;

    for (int8_t i = 0; i < payloadTypesCount; i++)
    {
        payloadTypes[i].locked = false;
        payloadTypes[i].updated = false;
        payloadTypes[i].data = PayloadData + offset;
        offset += payloadTypes[i].size;

        #if defined(UNIT_TEST)
        if (offset > sizeof(PayloadData)) {
            cout << "data not large enough\n";
        }
        #endif
    }
}

bool Telemetry::RXhandleUARTin(uint8_t data)
{
    switch(telemetry_state) {
        case TELEMETRY_IDLE:
            if (data == CRSF_ADDRESS_CRSF_RECEIVER || data == CRSF_SYNC_BYTE)
            {
                currentTelemetryByte = 0;
                telemetry_state = RECEIVING_LENGTH;
                CRSFinBuffer[0] = data;
            }
            else {
                return false;
            }

            break;
        case RECEIVING_LENGTH:
            if (data >= CRSF_MAX_PACKET_LEN)
            {
                telemetry_state = TELEMETRY_IDLE;
                return false;
            }
            else
            {
                telemetry_state = RECEIVING_DATA;
                CRSFinBuffer[CRSF_TELEMETRY_LENGTH_INDEX] = data;
            }

            break;
        case RECEIVING_DATA:
            CRSFinBuffer[currentTelemetryByte + CRSF_FRAME_NOT_COUNTED_BYTES] = data;
            currentTelemetryByte++;
            if (CRSFinBuffer[CRSF_TELEMETRY_LENGTH_INDEX] == currentTelemetryByte)
            {
                // exclude first bytes (sync byte + length), skip last byte (submitted crc)
                uint8_t crc = crsf_crc.calc(CRSFinBuffer + CRSF_FRAME_NOT_COUNTED_BYTES, CRSFinBuffer[CRSF_TELEMETRY_LENGTH_INDEX] - CRSF_TELEMETRY_CRC_LENGTH);
                telemetry_state = TELEMETRY_IDLE;

                if (data == crc)
                {
                    AppendTelemetryPackage(CRSFinBuffer);
                    receivedPackages++;
                    return true;
                }
                #if defined(UNIT_TEST)
                if (data != crc)
                {
                    cout << "invalid " << (int)crc  << '\n';
                }
                #endif

                return false;
            }

            break;
    }

    return true;
}

bool Telemetry::AppendTelemetryPackage(uint8_t *package)
{
    const crsf_header_t *header = (crsf_header_t *) package;

    if(header->type != CRSF_FRAMETYPE_BATTERY_SENSOR && header->type != CRSF_FRAMETYPE_FLIGHT_MODE && header->type != CRSF_FRAMETYPE_ATTITUDE)
    {
        //DBGLN("TLM HEADER %d", header->type);
    }

    if (header->type == CRSF_FRAMETYPE_COMMAND && package[3] == 'b' && package[4] == 'l')
    {
        callBootloader = true;
        return true;
    }
    if (header->type == CRSF_FRAMETYPE_COMMAND && package[3] == 'b' && package[4] == 'd')
    {
        callEnterBind = true;
        return true;
    }
    if (header->type == CRSF_FRAMETYPE_COMMAND && package[3] == 'm' && package[4] == 'm')
    {
        callUpdateModelMatch = true;
        modelMatchId = package[5];
        return true;
    }
    if (header->type == CRSF_FRAMETYPE_DEVICE_PING && package[CRSF_TELEMETRY_TYPE_INDEX + 1] == CRSF_ADDRESS_CRSF_RECEIVER)
    {
        sendDeviceFrame = true;
        return true;
    }
    
    if (header->type == CRSF_FRAMETYPE_MSP_REQ || header->type == CRSF_FRAMETYPE_MSP_RESP)
    {

    }

    uint8_t targetIndex = 0;
    bool targetFound = false;


    if (header->type >= CRSF_FRAMETYPE_DEVICE_PING)
    {
        const crsf_ext_header_t *extHeader = (crsf_ext_header_t *) package;

        if (header->type == CRSF_FRAMETYPE_ARDUPILOT_RESP)
        {
            // reserve last slot for adrupilot custom frame with the sub type status text: this is needed to make sure the important status messages are not lost
            if (package[CRSF_TELEMETRY_TYPE_INDEX + 1] == CRSF_AP_CUSTOM_TELEM_STATUS_TEXT)
            {
                targetIndex = payloadTypesCount - 1;
            }
            else
            {
                targetIndex = payloadTypesCount - 2;
            }
            targetFound = true;
        }
        else if (extHeader->orig_addr == CRSF_ADDRESS_FLIGHT_CONTROLLER)
        {
            targetIndex = payloadTypesCount - 2;
            targetFound = true;

            // larger msp resonses are sent in two chunks so special handling is needed so both get sent
            if (header->type == CRSF_FRAMETYPE_MSP_RESP || header->type == CRSF_FRAMETYPE_MSP_REQ)
            {
                uint8_t CRSFframeLen = CRSFinBuffer[CRSF_TELEMETRY_LENGTH_INDEX] +2;

                // DBG("UART->RESP: %d VAL:", CRSFframeLen);
                // char buf[CRSFframeLen * 3];
                // for (size_t i = 0; i < CRSFframeLen; i++)
                //     sprintf(&buf[3 * i], " %02hhX", package[i]);
                // DBGLN(buf);

                crsf.crsf2msp.parse(package);

                // if (crsf.crsf2msp.isFrameReady())
                // {
                //     uint32_t length = crsf.crsf2msp.getFrameLen();
                //     const uint8_t *frame = crsf.crsf2msp.getFrame();
                //     uint8_t buffer[length];
                //     memcpy(buffer, frame, length);

                //     if (crsf.msp2crsf.validate(buffer, length))
                //     {
                //         MSP2WIFI((const char *)buffer, length);
                //         crsf.crsf2msp.reset();
                //         DBGLN("$MSP RESP L: %d", length);
                //     }
                //     else
                //     {
                //         DBGLN("Frame Validation Err");
                //     }

                    
                //     // char buf[length * 3];
                //     // uint32_t i = 0;
                //     // for (i = 0; i < length; i++)
                //     //     sprintf(&buf[3 * i], " %02hhX", frame[i]);
                //     // DBGLN(buf);

                    

                //     return true;
                // }
                // there is already another response stored
                if (payloadTypes[targetIndex].updated)
                {
                    // use other slot
                    targetIndex = payloadTypesCount - 1;
                }

                // if both slots are taked do not overwrite other data since the first chunk would be lost
                if (payloadTypes[targetIndex].updated)
                {
                    targetFound = false;
                }
            }
        }
        else
        {
            targetIndex = payloadTypesCount - 1;
            targetFound = true;
        }
    }
    else
    {
        for (int8_t i = 0; i < payloadTypesCount - 2; i++)
        {
            if (header->type == payloadTypes[i].type)
            {
                if (!payloadTypes[i].locked && CRSF_FRAME_SIZE(package[CRSF_TELEMETRY_LENGTH_INDEX]) <= payloadTypes[i].size)
                {
                    targetIndex = i;
                    targetFound = true;
                }
                #if defined(UNIT_TEST)
                else if (CRSF_FRAME_SIZE(package[CRSF_TELEMETRY_LENGTH_INDEX]) > payloadTypes[i].size)
                {
                    cout << "buffer not large enough for type " << (int)payloadTypes[i].type  << " with size " << (int)payloadTypes[i].size << " would need " << CRSF_FRAME_SIZE(package[CRSF_TELEMETRY_LENGTH_INDEX]) << '\n';
                }
                #endif
                break;
            }
        }
    }

    if (targetFound)
    {
        memcpy(payloadTypes[targetIndex].data, package, CRSF_FRAME_SIZE(package[CRSF_TELEMETRY_LENGTH_INDEX]));
        payloadTypes[targetIndex].updated = true;
    }

    return targetFound;
}
#endif
