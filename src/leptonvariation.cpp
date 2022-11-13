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

    const uvc_extension_unit_t *units = uvc_get_extension_units(devh);
    while (units)
    {
        printf("Found extension unit ID %d, controls: %08llx, GUID:", units->bUnitID, units->bmControls);
        for (int i = 0; i < 16; i++)
            printf(" %02x", units->guidExtensionCode[i]);
        printf("\n");
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
    return QString::fromLatin1(partNumber.value, LEP_SYS_MAX_SERIAL_NUMBER_CHAR_SIZE);
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
