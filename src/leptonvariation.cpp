#include <QMutexLocker>
#include <QTimer>
#include "leptonvariation.h"
#include "uvc_sdk.h"
#include "LEPTON_SDK.h"
#include "LEPTON_OEM.h"
#include "LEPTON_RAD.h"
#include "LEPTON_SYS.h"
#include "LEPTON_VID.h"

#define LEP_CID_AGC_MODULE (0x0100)
#define LEP_CID_OEM_MODULE (0x0800)
#define LEP_CID_RAD_MODULE (0x0E00)
#define LEP_CID_SYS_MODULE (0x0200)
#define LEP_CID_VID_MODULE (0x0300)

typedef enum {
  VC_CONTROL_XU_LEP_AGC_ID = 3,
  VC_CONTROL_XU_LEP_OEM_ID,
  VC_CONTROL_XU_LEP_RAD_ID,
  VC_CONTROL_XU_LEP_SYS_ID,
  VC_CONTROL_XU_LEP_VID_ID,
  VC_CONTROL_XU_I2C_ID = 0x80,
  VC_CONTROL_XU_LEP_CUST_ID = 0xfe,
} VC_TERMINAL_ID;

#define QML_REGISTER_ENUM(name) \
    qmlRegisterUncreatableType<LEP::QE_##name>("GetThermal", 1,0, "LEP_" #name, "You can't create enumeration " #name); \
    qRegisterMetaType<LEP::QE_##name::E>("LEP_" #name);

void registerLeptonVariationQmlTypes()
{
    QML_REGISTER_ENUM(PCOLOR_LUT_E)
    QML_REGISTER_ENUM(POLARITY_E)
    QML_REGISTER_ENUM(VID_SBNUC_ENABLE_E)
    QML_REGISTER_ENUM(AGC_ENABLE_E)
    QML_REGISTER_ENUM(AGC_POLICY_E)
    QML_REGISTER_ENUM(AGC_HEQ_SCALE_FACTOR_E)
    QML_REGISTER_ENUM(RAD_TLINEAR_RESOLUTION_E)
    QML_REGISTER_ENUM(SYS_GAIN_MODE_E)
}

LeptonVariation::LeptonVariation(uvc_context_t *ctx,
                                 uvc_device_t *dev,
                                 uvc_device_handle_t *devh)
    : ctx(ctx)
    , dev(dev)
    , devh(devh)
    , m_mutex()
{
    printf("Initializing lepton SDK with UVC backend...\n");

    uvc_get_device_descriptor(dev, &desc);
    printf("Using %s %s with firmware %s\n", desc->manufacturer, desc->product, desc->serialNumber);

    m_portDesc.portID = 0;
    m_portDesc.portType = LEP_CCI_UVC;
    m_portDesc.userPtr = this;
    LEP_OpenPort(m_portDesc.portID,
                 m_portDesc.portType,
                 0,
                 &m_portDesc);
    printf("OK\n");

    supportsGenericI2C = false;
    const uvc_extension_unit_t *units = uvc_get_extension_units(devh);
    while (units)
    {
        printf("Found extension unit ID %d, controls: %08llx, GUID:", units->bUnitID, units->bmControls);
        for (int i = 0; i < 16; i++)
            printf(" %02x", units->guidExtensionCode[i]);
        printf("\n");
        if (units->bUnitID == 0xfe && (units->bmControls & (1<<6)))
            supportsGenericI2C = true;
        units = units->next;
    }

    const uvc_format_desc_t *desc = uvc_get_format_descs(devh);
    while (desc != NULL)
    {
        int width, height;
        width = desc->frame_descs[0].wWidth;
        height = desc->frame_descs[0].wHeight;
        m_sensorSize = QSize(width, height);
        break;
    }

    LEP_GetOemSoftwareVersion(&m_portDesc, &swVers);
    LEP_GetOemFlirPartNumber(&m_portDesc, &partNumber);
    serialNumber = pget<uint64_t, uint64_t>(LEP_GetSysFlirSerialNumber);

    LEP_GetRadSpotmeterRoi(&m_portDesc, &m_spotmeterRoi);

    this->setObjectName("LeptonVariation");

    m_periodicTimer = new QTimer(this);
    connect(m_periodicTimer, SIGNAL(timeout()), this, SLOT(updateSpotmeter()));
    m_periodicTimer->start(1000);

    printf("I2C for additional devices supported by firmware: %d\n", supportsGenericI2C);

    EnumerateMLX90614();
}

LeptonVariation::~LeptonVariation()
{
    uvc_free_device_descriptor(desc);
}

const AbstractCCInterface& LeptonVariation::operator =(const AbstractCCInterface&)
{
    return LeptonVariation(ctx, dev, devh);
}

const QString LeptonVariation::getSysFlirSerialNumber()
{
    return QString::asprintf("%08llx", serialNumber);
}

const QString LeptonVariation::getOemFlirPartNumber()
{
    if (partNumber.value[LEP_SYS_MAX_SERIAL_NUMBER_CHAR_SIZE-1] != 0)
        return QString::fromLatin1(partNumber.value, LEP_SYS_MAX_SERIAL_NUMBER_CHAR_SIZE);
    else
        return QString::fromLatin1(partNumber.value, -1);  // ignores trailing zeroes
}

const QString LeptonVariation::getOemGppSoftwareVersion()
{
    return QString::asprintf("%d.%d.%d", swVers.gpp_major, swVers.gpp_minor, swVers.gpp_build);
}

const QString LeptonVariation::getOemDspSoftwareVersion()
{
    return QString::asprintf("%d.%d.%d", swVers.dsp_major, swVers.dsp_minor, swVers.dsp_build);
}

const QString LeptonVariation::getPtFirmwareVersion() const
{
    return QString::asprintf("%s", desc->serialNumber);
}

bool LeptonVariation::getSupportsHwPseudoColor() const
{
    return getSupportsRuntimeAgcChange() || !getPtFirmwareVersion().contains("Y16");
}

bool LeptonVariation::getSupportsRadiometry()
{
    bool runtimeAgc = getSupportsRuntimeAgcChange();
    bool y16Firmware = getPtFirmwareVersion().contains("Y16");
    bool radiometricLepton = getOemFlirPartNumber().contains("500-0763-01")
            || getOemFlirPartNumber().contains("500-0771-01");
    return (runtimeAgc || y16Firmware) && radiometricLepton;
}

bool LeptonVariation::getSupportsRuntimeAgcChange() const
{
    return !getPtFirmwareVersion().startsWith("v0");
}

const QVideoSurfaceFormat LeptonVariation::getDefaultFormat()
{
    if (!getSupportsHwPseudoColor() || getSupportsRadiometry())
    {
        return QVideoSurfaceFormat(m_sensorSize, QVideoFrame::Format_Y16);
    }
    else
    {
        return QVideoSurfaceFormat(m_sensorSize, QVideoFrame::Format_RGB24);
    }
}

void LeptonVariation::updateSpotmeter()
{
    emit radSpotmeterInKelvinX100Changed();

    if (hasMLX90614)
    {
        if (ReadMLX90614AmbientTemperature(NULL, &ambientTemperatureMLX90614) == LEP_OK
            && ReadMLX90614ObjectTemperature(NULL, &objectTemperatureMLX90614) == LEP_OK)
        {
            printf("MLX90614 reports %.2f °C (ambient) and %.2f °C (object)\n",
                ambientTemperatureMLX90614 - 273.15, objectTemperatureMLX90614 - 273.15);
            emit irThermometerInKelvinChanged();
            emit irThermometerAmbientInKelvinChanged();
        }
    }
}

unsigned int LeptonVariation::getRadSpotmeterObjInKelvinX100()
{
    LEP_RAD_SPOTMETER_OBJ_KELVIN_T spotmeterObj;
    if (LEP_GetRadSpotmeterObjInKelvinX100(&m_portDesc, &spotmeterObj) == LEP_OK)
        return spotmeterObj.radSpotmeterValue;
    else
        return 0;
}

void LeptonVariation::setRadSpotmeterRoi(const QRect& roi)
{
    LEP_RAD_ROI_T newSpot = {
        static_cast<unsigned short>(roi.y()),
        static_cast<unsigned short>(roi.x()),
        static_cast<unsigned short>(roi.y() + roi.height()),
        static_cast<unsigned short>(roi.x() + roi.width())
    };

    if (LEP_SetRadSpotmeterRoi(&m_portDesc, newSpot) != LEP_OK) {
        printf("LEP_SetRadSpotmeterRoi failed");
        return;
    }

    m_spotmeterRoi = newSpot;
    emit radSpotmeterRoiChanged();
    emit radSpotmeterInKelvinX100Changed();
}

void LeptonVariation::performFfc()
{
    //LEP_RunOemFFC(&m_portDesc);
    LEP_RunSysFFCNormalization(&m_portDesc);
}

int LeptonVariation::leptonCommandIdToUnitId(LEP_COMMAND_ID commandID)
{
    int unit_id;

    switch (commandID & 0x3f00) // Ignore upper 2 bits including OEM bit
    {
    case LEP_CID_AGC_MODULE:
        unit_id = VC_CONTROL_XU_LEP_AGC_ID;
        break;

    case LEP_CID_OEM_MODULE:
        unit_id = VC_CONTROL_XU_LEP_OEM_ID;
        break;

    case LEP_CID_RAD_MODULE:
        unit_id = VC_CONTROL_XU_LEP_RAD_ID;
        break;

    case LEP_CID_SYS_MODULE:
        unit_id = VC_CONTROL_XU_LEP_SYS_ID;
        break;

    case LEP_CID_VID_MODULE:
        unit_id = VC_CONTROL_XU_LEP_VID_ID;
        break;

    default:
        return LEP_RANGE_ERROR;
    }

    return unit_id;
}

LEP_RESULT LeptonVariation::UVC_GetAttribute(LEP_COMMAND_ID commandID,
                            LEP_ATTRIBUTE_T_PTR attributePtr,
                            LEP_UINT16 attributeWordLength)
{
    int unit_id;
    int control_id;
    int result;

    unit_id = leptonCommandIdToUnitId(commandID);
    if (unit_id < 0)
        return (LEP_RESULT)unit_id;

    control_id = ((commandID & 0x00ff) >> 2) + 1;

    // Size in 16-bit words needs to be in bytes
    attributeWordLength *= 2;

    QMutexLocker lock(&m_mutex);
    result = uvc_get_ctrl(devh, unit_id, control_id, attributePtr, attributeWordLength, UVC_GET_CUR);
    if (result != attributeWordLength)
    {
        printf("UVC_GetAttribute failed: %d\n", result);
        return LEP_COMM_ERROR_READING_COMM;
    }

    return LEP_OK;
}

LEP_RESULT LeptonVariation::UVC_SetAttribute(LEP_COMMAND_ID commandID,
                            LEP_ATTRIBUTE_T_PTR attributePtr,
                            LEP_UINT16 attributeWordLength)
{
    int unit_id;
    int control_id;
    int result;

    unit_id = leptonCommandIdToUnitId(commandID);
    if (unit_id < 0)
        return (LEP_RESULT)unit_id;

    control_id = ((commandID & 0x00ff) >> 2) + 1;

    // Size in 16-bit words needs to be in bytes
    attributeWordLength *= 2;

    QMutexLocker lock(&m_mutex);
    result = uvc_set_ctrl(devh, unit_id, control_id, attributePtr, attributeWordLength);
    if (result != attributeWordLength)
    {
        printf("UVC_SetAttribute failed: %d\n", result);
        return LEP_COMM_ERROR_READING_COMM;
    }

    return LEP_OK;
}

LEP_RESULT LeptonVariation::UVC_RunCommand(LEP_COMMAND_ID commandID)
{
    int unit_id;
    int control_id;
    int result;

    unit_id = leptonCommandIdToUnitId(commandID);
    if (unit_id < 0)
        return (LEP_RESULT)unit_id;

    control_id = ((commandID & 0x00ff) >> 2) + 1;

    QMutexLocker lock(&m_mutex);
    result = uvc_set_ctrl(devh, unit_id, control_id, &control_id, 1);
    if (result != 1)
    {
        printf("UVC_RunCommand failed: %d\n", result);
        return LEP_COMM_ERROR_READING_COMM;
    }

    return LEP_OK;
}

enum CUST_COMTROL_IDS {
	CUST_CONTROL_COMMAND=0,
	CUST_CONTROL_GET,
	CUST_CONTROL_SET,
	CUST_CONTROL_RUN,
	CUST_CONTROL_DIRECT_WRITE,
	CUST_CONTROL_DIRECT_READ,
    CUST_CONTROL_I2C_WRITEREAD,
	CUST_CONTROL_END
};

LEP_RESULT LeptonVariation::UVC_CustomRead(void* attributePtr, int length)
{
    int result;

    if (length != 2+2+512)
        return LEP_ERROR;

    QMutexLocker lock(&m_mutex);
    result = uvc_get_ctrl(devh, VC_CONTROL_XU_LEP_CUST_ID, CUST_CONTROL_COMMAND+1, attributePtr, length, UVC_GET_CUR);
    if (result != length)
    {
        printf("UVC_CustomRead failed: %d\n", result);
        return LEP_COMM_ERROR_READING_COMM;
    }

    return LEP_OK;
}

LEP_RESULT LeptonVariation::UVC_CustomWrite(const void* attributePtr, int length)
{
    int result;

    if (length != 2+2+512)
        return LEP_ERROR;

    QMutexLocker lock(&m_mutex);
    result = uvc_set_ctrl(devh, VC_CONTROL_XU_LEP_CUST_ID, CUST_CONTROL_COMMAND+1, (void*)attributePtr, length);
    if (result != length)
    {
        printf("UVC_CustomRead failed: %d\n", result);
        return LEP_COMM_ERROR_READING_COMM;
    }

    return LEP_OK;
}

LEP_RESULT LeptonVariation::UVC_I2CWriteRead(uint8_t i2cAddress,
                                             const void* writeData,
                                             int writeLength,
                                             void* readData,
                                             int readLength,
                                             LEP_RESULT* i2cResult)
{
    int result;
    struct { uint16_t address; int16_t lengthWrite, lengthRead; uint8_t data[510]; } __attribute__((packed)) custom_uvc;
    struct { LEP_RESULT result; uint8_t data[512]; } __attribute__((packed)) custom_response;

    if (writeLength > (int)sizeof(custom_uvc.data) || readLength > (int)sizeof(custom_response.data) || writeLength < -1 || readLength < -1)
        return LEP_ERROR;

    if (!supportsGenericI2C)
        return LEP_FUNCTION_NOT_SUPPORTED;

    QMutexLocker lock(&m_mutex);

    custom_uvc.address = i2cAddress;
    custom_uvc.lengthWrite = (int16_t)writeLength;
    custom_uvc.lengthRead = (int16_t)readLength;
    memset(custom_uvc.data, 0, sizeof(custom_uvc.data));
    if (writeLength > 0)
        memcpy(custom_uvc.data, writeData, writeLength);

    LEP_RESULT lep_result = UVC_CustomWrite(&custom_uvc, sizeof(custom_uvc));
    if (lep_result != LEP_OK)
    {
        printf("UVC_I2CWriteRead failed in UVC_CustomWrite: %d\n", lep_result);
        return lep_result;
    }


    result = uvc_get_ctrl(devh, VC_CONTROL_XU_LEP_CUST_ID, CUST_CONTROL_I2C_WRITEREAD+1, &custom_response, sizeof(custom_response), UVC_GET_CUR);
    if (result != sizeof(custom_response))
    {
        printf("UVC_I2CWriteRead failed: %d, should be %lu, %04x %02x %02x %02x %02x\n", result, sizeof(custom_response),
            custom_response.result, custom_response.data[0], custom_response.data[1], custom_response.data[2], custom_response.data[3]);
        return LEP_COMM_ERROR_READING_COMM;
    }

    if (custom_response.result == LEP_ERROR) {
        printf("UVC_I2CWriteRead got LEP_ERROR, %04x %02x %02x %02x %02x\n",
            custom_response.result, custom_response.data[0], custom_response.data[1], custom_response.data[2], custom_response.data[3]);
    }

    *i2cResult = custom_response.result;
    if (readLength > 0)
        memcpy(readData, custom_response.data, readLength);

    return LEP_OK;
}

LEP_RESULT LeptonVariation::UVC_I2CWrite(uint8_t i2cAddress,
                            const void* writeData,
                            int writeLength,
                            LEP_RESULT* i2cResult)
{
    uint8_t dummy;
    return UVC_I2CWriteRead(i2cAddress, writeData, writeLength, &dummy, -1, i2cResult);
}

LEP_RESULT LeptonVariation::UVC_I2CRead(uint8_t i2cAddress,
                            void* readData,
                            int readLength,
                            LEP_RESULT* i2cResult)
{
    return UVC_I2CWriteRead(i2cAddress, "", -1, readData, readLength, i2cResult);
}

LEP_RESULT LeptonVariation::UVC_I2CScan(bool addressPresent[128], bool verbose)
{
    LEP_RESULT first_unusual_result = LEP_OK;

    for (int i=0; i<128; i++) {
        LEP_RESULT result, result2;
        char buf[1];
        // read of length 0 or 1 doesn't work well when Lepton and MLX are connected (hangs after talking to Lepton)
        //result = UVC_I2CRead((uint8_t)i, buf, 0, &result2);
        // use a write access with length 0, instead
        result = UVC_I2CWrite((uint8_t)i, buf, 0, &result2);
        if (result != LEP_OK)
        {
            if (verbose)
                printf("\nERROR in UVC_I2CScan\n");
            return result;
        }

        addressPresent[i] = (result2 == LEP_OK);

        if (verbose)
        {
            if (i%16 == 0)
                printf("%02x:", i);

            if (result2 == LEP_OK)
                printf(" %02x", i);
            else if (result2 == LEP_ERROR_I2C_NACK_RECEIVED)
                printf(" --");
            else
            {
                printf(" ??");
                if (first_unusual_result == LEP_OK)
                    first_unusual_result = result2;
            }

            if (i%16 == 15)
                printf("\n");
        }
    }

    if (verbose && first_unusual_result != LEP_OK)
        printf("Result for first ?? is %d.\n", first_unusual_result);

    return LEP_OK;
}

LEP_RESULT LeptonVariation::ReadFromMLX90614(uint8_t command, uint16_t* out)
{
    // 0x00..0x1f is RAM, 0x20..0x3f is EEPROM. Only these parts can be accessed by this read function.
    if (command > 0x40)
        return LEP_ERROR;

    uint8_t cmd[] = { command };
    uint8_t reply[3];
    LEP_RESULT i2cResult;
    LEP_RESULT result = UVC_I2CWriteRead(0x5a, cmd, 1, reply, 3, &i2cResult);
    if (result != LEP_OK)
    {
        printf("Couldn't send I2C request to PureThermal.\n");
        return result < 0 ? result : LEP_ERROR;
    }
    else if (i2cResult != LEP_OK)
    {
        printf("I2C communication to MLX90614 didn't go as expected: %d\n", i2cResult);

        if (errorsForMLX90614 < 5)
        {
            errorsForMLX90614++;
            if (errorsForMLX90614 == 5)
            {
                printf("Giving up on MLX90614 because there were too many errors.\n");
                hasMLX90614 = false;
                emit irThermometerAvailableChanged();
            }
        }

        return result < 0 ? result : LEP_ERROR_I2C_FAIL;
    }
    else
    {
        if (errorsForMLX90614 > -5)
            errorsForMLX90614--;

        //FIXME We should compare the checksum in the third byte.
        *out = reply[0] | (reply[1] << 8);
        return LEP_OK;
    }
}

LEP_RESULT LeptonVariation::ReadMLX90614AmbientTemperature(uint16_t* raw, float* kelvin, bool force)
{
    LEP_RESULT result;

    if (!hasMLX90614 && !force)
        return LEP_FUNCTION_NOT_SUPPORTED;

    uint16_t value;
    result = ReadFromMLX90614(0x6, &value);
    if (result < 0)
        return result;

    if (value & 0x8000)
        // sensor tells us that the value isn't valid
        return LEP_DATA_OUT_OF_RANGE_ERROR;

    if (raw)
        *raw = value;
    if (kelvin)
        *kelvin = value * 0.02;

    return LEP_OK;
}

LEP_RESULT LeptonVariation::ReadMLX90614ObjectTemperature(uint16_t* raw, float* kelvin, bool force)
{
    LEP_RESULT result;

    if (!hasMLX90614 && !force)
        return LEP_FUNCTION_NOT_SUPPORTED;

    uint16_t value;
    result = ReadFromMLX90614(0x7, &value);
    if (result < 0)
        return result;

    if (value & 0x8000)
        // sensor tells us that the value isn't valid
        return LEP_DATA_OUT_OF_RANGE_ERROR;

    if (raw)
        *raw = value;
    if (kelvin)
        *kelvin = value * 0.02;

    return LEP_OK;
}

LEP_RESULT LeptonVariation::EnumerateMLX90614()
{
    LEP_RESULT result, i2cResult;
    uint8_t buf[3];

    hasMLX90614 = false;
    errorsForMLX90614 = 0;
    ambientTemperatureMLX90614 = -300;
    objectTemperatureMLX90614 = -300;

    if (!supportsGenericI2C)
        return LEP_OK;

    result = UVC_I2CWrite(0x5a, buf, 0, &i2cResult);
    if (result != LEP_OK)
    {
        return result;
    }
    else if (i2cResult != LEP_OK)
    {
        // no reply from sensors -> probably no sensor is present
        return LEP_OK;
    }

    result = UVC_I2CWrite(0x5a+1, buf, 0, &i2cResult);
    if (result != LEP_OK)
    {
        return result;
    }
    else if (i2cResult == LEP_OK)
    {
        // unexpected reply from an address that shouldn't have any device
        // -> Let's play it save (i.e. assume no sensors is present) to not confuse users in case of weird I2C behaviour.
        return LEP_OK;
    }

    // Melexis hasn't documented any way of detecting an MLX90614 (or even to tell apart which type it is).
    // Therefore, we look at some default values in EEPROM. There is no reason to change them when using
    // the sensor in I2C mode so we assume that users. These values seem to be the same even non-original
    // devices that differ in other regards, namely how they calculate the checksum.
    uint16_t eeprom0, eeprom1;
    result = ReadFromMLX90614(0x20, &eeprom0);
    if (result != LEP_OK)
        return result;
    result = ReadFromMLX90614(0x21, &eeprom1);
    if (result != LEP_OK)
        return result;
    if (eeprom0 != 0x9993 || eeprom1 != 0x62e3)
    {
        printf("We found some device at I2C address 0x5a but we got unexpected values when reading EEPROM cells 0 and 1. Therefore, we assume that it is not an MLX90614 sensor.\n");
        return LEP_OK;
    }

    float ambientTemperature, objectTemperature;
    result = ReadMLX90614AmbientTemperature(NULL, &ambientTemperature, true);
    if (result != LEP_OK)
    {
        printf("Cannot read ambient temperature from MLX90614: %d\n", result);
        return LEP_OK;
    }

    result = ReadMLX90614ObjectTemperature(NULL, &objectTemperature, true);
    if (result != LEP_OK)
    {
        printf("Cannot read object temperature from MLX90614: %d\n", result);
        return LEP_OK;
    }

    //NOTE Both ranges are a bit wider than what is supported according to the datasheet.
    float ambientTemperatureCelsius = ambientTemperature - 273.15, objectTemperatureCelsius = objectTemperature - 273.15;
    if (ambientTemperatureCelsius < -60 || ambientTemperatureCelsius > 150 || objectTemperatureCelsius < -100 || objectTemperatureCelsius > 500)
    {
        printf("We got unexpected temperatures from MLX90614: ambient %.2f °C, object %.2f °C\n", ambientTemperatureCelsius, objectTemperatureCelsius);
        return LEP_OK;
    }

    printf("We found an MLX90614 connected to the PureThermal board. Current temperatures are %.2f °C (ambient, i.e. the sensor itself) and %.2f °C (object).\n",
        ambientTemperatureCelsius, objectTemperatureCelsius);

    hasMLX90614 = true;
    ambientTemperatureMLX90614 = ambientTemperature;
    objectTemperatureMLX90614 = objectTemperature;

    emit irThermometerInKelvinChanged();
    emit irThermometerAmbientInKelvinChanged();
    emit irThermometerAvailableChanged();

    return LEP_OK;
}

/* --------------------------------------------------------------------- */
/* -------- static wrapper functions for use by Lepton SDK only -------- */
/* --------------------------------------------------------------------- */

LEP_RESULT UVC_GetAttribute(LEP_CAMERA_PORT_DESC_T_PTR portDescPtr,
                            LEP_COMMAND_ID commandID,
                            LEP_ATTRIBUTE_T_PTR attributePtr,
                            LEP_UINT16 attributeWordLength)
{
    return static_cast<LeptonVariation*>(portDescPtr->userPtr) ->
            UVC_GetAttribute(commandID, attributePtr, attributeWordLength);
}

LEP_RESULT UVC_SetAttribute(LEP_CAMERA_PORT_DESC_T_PTR portDescPtr,
                            LEP_COMMAND_ID commandID,
                            LEP_ATTRIBUTE_T_PTR attributePtr,
                            LEP_UINT16 attributeWordLength)
{
    return static_cast<LeptonVariation*>(portDescPtr->userPtr) ->
            UVC_SetAttribute(commandID, attributePtr, attributeWordLength);
}

LEP_RESULT UVC_RunCommand(LEP_CAMERA_PORT_DESC_T_PTR portDescPtr,
                          LEP_COMMAND_ID commandID)
{
    return static_cast<LeptonVariation*>(portDescPtr->userPtr) ->
            UVC_RunCommand(commandID);
}
