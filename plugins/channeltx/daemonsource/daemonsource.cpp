///////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2018 Edouard Griffiths, F4EXB                                   //
//                                                                               //
// This program is free software; you can redistribute it and/or modify          //
// it under the terms of the GNU General Public License as published by          //
// the Free Software Foundation as version 3 of the License, or                  //
//                                                                               //
// This program is distributed in the hope that it will be useful,               //
// but WITHOUT ANY WARRANTY; without even the implied warranty of                //
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                  //
// GNU General Public License V3 for more details.                               //
//                                                                               //
// You should have received a copy of the GNU General Public License             //
// along with this program. If not, see <http://www.gnu.org/licenses/>.          //
///////////////////////////////////////////////////////////////////////////////////

#include <sys/time.h>
#include <unistd.h>
#include <boost/crc.hpp>
#include <boost/cstdint.hpp>

#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QBuffer>

#include "SWGChannelSettings.h"
#include "SWGChannelReport.h"
#include "SWGDaemonSourceReport.h"

#include "dsp/devicesamplesink.h"
#include "device/devicesinkapi.h"
#include "dsp/upchannelizer.h"
#include "dsp/threadedbasebandsamplesource.h"

#include "daemonsourcethread.h"
#include "daemonsource.h"

MESSAGE_CLASS_DEFINITION(DaemonSource::MsgSampleRateNotification, Message)
MESSAGE_CLASS_DEFINITION(DaemonSource::MsgConfigureDaemonSource, Message)
MESSAGE_CLASS_DEFINITION(DaemonSource::MsgQueryStreamData, Message)
MESSAGE_CLASS_DEFINITION(DaemonSource::MsgReportStreamData, Message)

const QString DaemonSource::m_channelIdURI = "sdrangel.channeltx.daemonsource";
const QString DaemonSource::m_channelId ="DaemonSource";

DaemonSource::DaemonSource(DeviceSinkAPI *deviceAPI) :
    ChannelSourceAPI(m_channelIdURI),
    m_deviceAPI(deviceAPI),
    m_sourceThread(0),
    m_running(false),
    m_nbCorrectableErrors(0),
    m_nbUncorrectableErrors(0)
{
    setObjectName(m_channelId);

    connect(&m_dataQueue, SIGNAL(dataBlockEnqueued()), this, SLOT(handleData()), Qt::QueuedConnection);
    m_cm256p = m_cm256.isInitialized() ? &m_cm256 : 0;
    m_currentMeta.init();

    m_channelizer = new UpChannelizer(this);
    m_threadedChannelizer = new ThreadedBasebandSampleSource(m_channelizer, this);
    m_deviceAPI->addThreadedSource(m_threadedChannelizer);
    m_deviceAPI->addChannelAPI(this);

    m_networkManager = new QNetworkAccessManager();
    connect(m_networkManager, SIGNAL(finished(QNetworkReply*)), this, SLOT(networkManagerFinished(QNetworkReply*)));
}

DaemonSource::~DaemonSource()
{
    disconnect(m_networkManager, SIGNAL(finished(QNetworkReply*)), this, SLOT(networkManagerFinished(QNetworkReply*)));
    delete m_networkManager;
    m_deviceAPI->removeChannelAPI(this);
    m_deviceAPI->removeThreadedSource(m_threadedChannelizer);
    delete m_threadedChannelizer;
    delete m_channelizer;
}

void DaemonSource::pull(Sample& sample)
{
    m_dataReadQueue.readSample(sample, true); // true is scale for Tx
}

void DaemonSource::pullAudio(int nbSamples)
{
    (void) nbSamples;
}

void DaemonSource::start()
{
    qDebug("DaemonSource::start");

    if (m_running) {
        stop();
    }

    m_sourceThread = new DaemonSourceThread(&m_dataQueue);
    m_sourceThread->startStop(true);
    m_sourceThread->dataBind(m_settings.m_dataAddress, m_settings.m_dataPort);
    m_running = true;
}

void DaemonSource::stop()
{
    qDebug("DaemonSource::stop");

    if (m_sourceThread != 0)
    {
        m_sourceThread->startStop(false);
        m_sourceThread->deleteLater();
        m_sourceThread = 0;
    }

    m_running = false;
}

void DaemonSource::setDataLink(const QString& dataAddress, uint16_t dataPort)
{
    DaemonSourceSettings settings = m_settings;
    settings.m_dataAddress = dataAddress;
    settings.m_dataPort = dataPort;

    MsgConfigureDaemonSource *msg = MsgConfigureDaemonSource::create(settings, false);
    m_inputMessageQueue.push(msg);
}

bool DaemonSource::handleMessage(const Message& cmd)
{
    if (UpChannelizer::MsgChannelizerNotification::match(cmd))
    {
        UpChannelizer::MsgChannelizerNotification& notif = (UpChannelizer::MsgChannelizerNotification&) cmd;
        qDebug() << "DaemonSource::handleMessage: MsgChannelizerNotification:"
                << " basebandSampleRate: " << notif.getBasebandSampleRate()
                << " outputSampleRate: " << notif.getSampleRate()
                << " inputFrequencyOffset: " << notif.getFrequencyOffset();

        if (m_guiMessageQueue)
        {
            MsgSampleRateNotification *msg = MsgSampleRateNotification::create(notif.getBasebandSampleRate());
            m_guiMessageQueue->push(msg);
        }

        return true;
    }
    else if (MsgConfigureDaemonSource::match(cmd))
    {
        MsgConfigureDaemonSource& cfg = (MsgConfigureDaemonSource&) cmd;
        qDebug() << "MsgConfigureDaemonSource::handleMessage: MsgConfigureDaemonSource";
        applySettings(cfg.getSettings(), cfg.getForce());

        return true;
    }
    else if (MsgQueryStreamData::match(cmd))
    {
        if (m_guiMessageQueue)
        {
            struct timeval tv;
            gettimeofday(&tv, 0);

            MsgReportStreamData *msg = MsgReportStreamData::create(
                    tv.tv_sec,
                    tv.tv_usec,
                    m_dataReadQueue.size(),
                    m_dataReadQueue.length(),
                    m_dataReadQueue.readSampleCount(),
                    m_nbCorrectableErrors,
                    m_nbUncorrectableErrors,
                    m_currentMeta.m_nbOriginalBlocks,
                    m_currentMeta.m_nbFECBlocks,
                    m_currentMeta.m_centerFrequency,
                    m_currentMeta.m_sampleRate);
            m_guiMessageQueue->push(msg);
        }

        return true;
    }
    return false;
}

QByteArray DaemonSource::serialize() const
{
    return m_settings.serialize();
}

bool DaemonSource::deserialize(const QByteArray& data)
{
    (void) data;
    if (m_settings.deserialize(data))
    {
        MsgConfigureDaemonSource *msg = MsgConfigureDaemonSource::create(m_settings, true);
        m_inputMessageQueue.push(msg);
        return true;
    }
    else
    {
        m_settings.resetToDefaults();
        MsgConfigureDaemonSource *msg = MsgConfigureDaemonSource::create(m_settings, true);
        m_inputMessageQueue.push(msg);
        return false;
    }
}

void DaemonSource::applySettings(const DaemonSourceSettings& settings, bool force)
{
    qDebug() << "DaemonSource::applySettings:"
            << " m_dataAddress: " << settings.m_dataAddress
            << " m_dataPort: " << settings.m_dataPort
            << " force: " << force;

    bool change = false;
    QList<QString> reverseAPIKeys;

    if ((m_settings.m_dataAddress != settings.m_dataAddress) || force)
    {
        reverseAPIKeys.append("dataAddress");
        change = true;
    }

    if ((m_settings.m_dataPort != settings.m_dataPort) || force)
    {
        reverseAPIKeys.append("dataPort");
        change = true;
    }

    if (change && m_sourceThread)
    {
        reverseAPIKeys.append("sourceThread");
        m_sourceThread->dataBind(settings.m_dataAddress, settings.m_dataPort);
    }

    if (settings.m_useReverseAPI)
    {
        bool fullUpdate = ((m_settings.m_useReverseAPI != settings.m_useReverseAPI) && settings.m_useReverseAPI) ||
                (m_settings.m_reverseAPIAddress != settings.m_reverseAPIAddress) ||
                (m_settings.m_reverseAPIPort != settings.m_reverseAPIPort) ||
                (m_settings.m_reverseAPIDeviceIndex != settings.m_reverseAPIDeviceIndex) ||
                (m_settings.m_reverseAPIChannelIndex != settings.m_reverseAPIChannelIndex);
        webapiReverseSendSettings(reverseAPIKeys, settings, fullUpdate || force);
    }

    m_settings = settings;
}

void DaemonSource::handleDataBlock(SDRDaemonDataBlock* dataBlock)
{
    (void) dataBlock;
    if (dataBlock->m_rxControlBlock.m_blockCount < SDRDaemonNbOrginalBlocks)
    {
        qWarning("DaemonSource::handleDataBlock: incomplete data block: not processing");
    }
    else
    {
        int blockCount = 0;

        for (int blockIndex = 0; blockIndex < 256; blockIndex++)
        {
            if ((blockIndex == 0) && (dataBlock->m_rxControlBlock.m_metaRetrieved))
            {
                m_cm256DescriptorBlocks[blockCount].Index = 0;
                m_cm256DescriptorBlocks[blockCount].Block = (void *) &(dataBlock->m_superBlocks[0].m_protectedBlock);
                blockCount++;
            }
            else if (dataBlock->m_superBlocks[blockIndex].m_header.m_blockIndex != 0)
            {
                m_cm256DescriptorBlocks[blockCount].Index = dataBlock->m_superBlocks[blockIndex].m_header.m_blockIndex;
                m_cm256DescriptorBlocks[blockCount].Block = (void *) &(dataBlock->m_superBlocks[blockIndex].m_protectedBlock);
                blockCount++;
            }
        }

        //qDebug("DaemonSource::handleDataBlock: frame: %u blocks: %d", dataBlock.m_rxControlBlock.m_frameIndex, blockCount);

        // Need to use the CM256 recovery
        if (m_cm256p &&(dataBlock->m_rxControlBlock.m_originalCount < SDRDaemonNbOrginalBlocks))
        {
            qDebug("DaemonSource::handleDataBlock: %d recovery blocks", dataBlock->m_rxControlBlock.m_recoveryCount);
            CM256::cm256_encoder_params paramsCM256;
            paramsCM256.BlockBytes = sizeof(SDRDaemonProtectedBlock); // never changes
            paramsCM256.OriginalCount = SDRDaemonNbOrginalBlocks;  // never changes

            if (m_currentMeta.m_tv_sec == 0) {
                paramsCM256.RecoveryCount = dataBlock->m_rxControlBlock.m_recoveryCount;
            } else {
                paramsCM256.RecoveryCount = m_currentMeta.m_nbFECBlocks;
            }

            // update counters
            if (dataBlock->m_rxControlBlock.m_originalCount < SDRDaemonNbOrginalBlocks - paramsCM256.RecoveryCount) {
                m_nbUncorrectableErrors += SDRDaemonNbOrginalBlocks - paramsCM256.RecoveryCount - dataBlock->m_rxControlBlock.m_originalCount;
            } else {
                m_nbCorrectableErrors += dataBlock->m_rxControlBlock.m_recoveryCount;
            }

            if (m_cm256.cm256_decode(paramsCM256, m_cm256DescriptorBlocks)) // CM256 decode
            {
                qWarning() << "DaemonSource::handleDataBlock: decode CM256 error:"
                        << " m_originalCount: " << dataBlock->m_rxControlBlock.m_originalCount
                        << " m_recoveryCount: " << dataBlock->m_rxControlBlock.m_recoveryCount;
            }
            else
            {
                for (int ir = 0; ir < dataBlock->m_rxControlBlock.m_recoveryCount; ir++) // restore missing blocks
                {
                    int recoveryIndex = SDRDaemonNbOrginalBlocks - dataBlock->m_rxControlBlock.m_recoveryCount + ir;
                    int blockIndex = m_cm256DescriptorBlocks[recoveryIndex].Index;
                    SDRDaemonProtectedBlock *recoveredBlock =
                            (SDRDaemonProtectedBlock *) m_cm256DescriptorBlocks[recoveryIndex].Block;
                    memcpy((void *) &(dataBlock->m_superBlocks[blockIndex].m_protectedBlock), recoveredBlock, sizeof(SDRDaemonProtectedBlock));
                    if ((blockIndex == 0) && !dataBlock->m_rxControlBlock.m_metaRetrieved) {
                        dataBlock->m_rxControlBlock.m_metaRetrieved = true;
                    }
                }
            }
        }

        // Validate block zero and retrieve its data
        if (dataBlock->m_rxControlBlock.m_metaRetrieved)
        {
            SDRDaemonMetaDataFEC *metaData = (SDRDaemonMetaDataFEC *) &(dataBlock->m_superBlocks[0].m_protectedBlock);
            boost::crc_32_type crc32;
            crc32.process_bytes(metaData, 20);

            if (crc32.checksum() == metaData->m_crc32)
            {
                if (!(m_currentMeta == *metaData))
                {
                    printMeta("DaemonSource::handleDataBlock", metaData);

                    if (m_currentMeta.m_sampleRate != metaData->m_sampleRate)
                    {
                        m_channelizer->configure(m_channelizer->getInputMessageQueue(), metaData->m_sampleRate, 0);
                        m_dataReadQueue.setSize(calculateDataReadQueueSize(metaData->m_sampleRate));
                    }
                }

                m_currentMeta = *metaData;
            }
            else
            {
                qWarning() << "DaemonSource::handleDataBlock: recovered meta: invalid CRC32";
            }
        }

        m_dataReadQueue.push(dataBlock); // Push into R/W buffer
    }
}

void DaemonSource::handleData()
{
    SDRDaemonDataBlock* dataBlock;

    while (m_running && ((dataBlock = m_dataQueue.pop()) != 0)) {
        handleDataBlock(dataBlock);
    }
}

void DaemonSource::printMeta(const QString& header, SDRDaemonMetaDataFEC *metaData)
{
    qDebug().noquote() << header << ": "
            << "|" << metaData->m_centerFrequency
            << ":" << metaData->m_sampleRate
            << ":" << (int) (metaData->m_sampleBytes & 0xF)
            << ":" << (int) metaData->m_sampleBits
            << ":" << (int) metaData->m_nbOriginalBlocks
            << ":" << (int) metaData->m_nbFECBlocks
            << "|" << metaData->m_tv_sec
            << ":" << metaData->m_tv_usec
            << "|";
}

uint32_t DaemonSource::calculateDataReadQueueSize(int sampleRate)
{
    // scale for 20 blocks at 48 kS/s. Take next even number.
    uint32_t maxSize = sampleRate / 2400;
    maxSize = (maxSize % 2 == 0) ? maxSize : maxSize + 1;
    qDebug("DaemonSource::calculateDataReadQueueSize: set max queue size to %u blocks", maxSize);
    return maxSize;
}

int DaemonSource::webapiSettingsGet(
        SWGSDRangel::SWGChannelSettings& response,
        QString& errorMessage)
{
    (void) errorMessage;
    response.setDaemonSourceSettings(new SWGSDRangel::SWGDaemonSourceSettings());
    response.getDaemonSourceSettings()->init();
    webapiFormatChannelSettings(response, m_settings);
    return 200;
}

int DaemonSource::webapiSettingsPutPatch(
        bool force,
        const QStringList& channelSettingsKeys,
        SWGSDRangel::SWGChannelSettings& response,
        QString& errorMessage)
{
    (void) errorMessage;
    DaemonSourceSettings settings = m_settings;

    if (channelSettingsKeys.contains("dataAddress")) {
        settings.m_dataAddress = *response.getDaemonSourceSettings()->getDataAddress();
    }
    if (channelSettingsKeys.contains("dataPort"))
    {
        int dataPort = response.getDaemonSourceSettings()->getDataPort();

        if ((dataPort < 1024) || (dataPort > 65535)) {
            settings.m_dataPort = 9090;
        } else {
            settings.m_dataPort = dataPort;
        }
    }
    if (channelSettingsKeys.contains("rgbColor")) {
        settings.m_rgbColor = response.getDaemonSourceSettings()->getRgbColor();
    }
    if (channelSettingsKeys.contains("title")) {
        settings.m_title = *response.getDaemonSourceSettings()->getTitle();
    }
    if (channelSettingsKeys.contains("useReverseAPI")) {
        settings.m_useReverseAPI = response.getDaemonSourceSettings()->getUseReverseApi() != 0;
    }
    if (channelSettingsKeys.contains("reverseAPIAddress")) {
        settings.m_reverseAPIAddress = *response.getDaemonSourceSettings()->getReverseApiAddress() != 0;
    }
    if (channelSettingsKeys.contains("reverseAPIPort")) {
        settings.m_reverseAPIPort = response.getDaemonSourceSettings()->getReverseApiPort();
    }
    if (channelSettingsKeys.contains("reverseAPIDeviceIndex")) {
        settings.m_reverseAPIDeviceIndex = response.getDaemonSourceSettings()->getReverseApiDeviceIndex();
    }
    if (channelSettingsKeys.contains("reverseAPIChannelIndex")) {
        settings.m_reverseAPIChannelIndex = response.getDaemonSourceSettings()->getReverseApiChannelIndex();
    }

    MsgConfigureDaemonSource *msg = MsgConfigureDaemonSource::create(settings, force);
    m_inputMessageQueue.push(msg);

    qDebug("DaemonSource::webapiSettingsPutPatch: forward to GUI: %p", m_guiMessageQueue);
    if (m_guiMessageQueue) // forward to GUI if any
    {
        MsgConfigureDaemonSource *msgToGUI = MsgConfigureDaemonSource::create(settings, force);
        m_guiMessageQueue->push(msgToGUI);
    }

    webapiFormatChannelSettings(response, settings);

    return 200;
}

int DaemonSource::webapiReportGet(
        SWGSDRangel::SWGChannelReport& response,
        QString& errorMessage)
{
    (void) errorMessage;
    response.setDaemonSourceReport(new SWGSDRangel::SWGDaemonSourceReport());
    response.getDaemonSourceReport()->init();
    webapiFormatChannelReport(response);
    return 200;
}

void DaemonSource::webapiFormatChannelSettings(SWGSDRangel::SWGChannelSettings& response, const DaemonSourceSettings& settings)
{
    if (response.getDaemonSourceSettings()->getDataAddress()) {
        *response.getDaemonSourceSettings()->getDataAddress() = settings.m_dataAddress;
    } else {
        response.getDaemonSourceSettings()->setDataAddress(new QString(settings.m_dataAddress));
    }

    response.getDaemonSourceSettings()->setDataPort(settings.m_dataPort);
    response.getDaemonSourceSettings()->setRgbColor(settings.m_rgbColor);

    if (response.getDaemonSourceSettings()->getTitle()) {
        *response.getDaemonSourceSettings()->getTitle() = settings.m_title;
    } else {
        response.getDaemonSourceSettings()->setTitle(new QString(settings.m_title));
    }

    response.getDaemonSourceSettings()->setUseReverseApi(settings.m_useReverseAPI ? 1 : 0);

    if (response.getDaemonSourceSettings()->getReverseApiAddress()) {
        *response.getDaemonSourceSettings()->getReverseApiAddress() = settings.m_reverseAPIAddress;
    } else {
        response.getDaemonSourceSettings()->setReverseApiAddress(new QString(settings.m_reverseAPIAddress));
    }

    response.getDaemonSourceSettings()->setReverseApiPort(settings.m_reverseAPIPort);
    response.getDaemonSourceSettings()->setReverseApiDeviceIndex(settings.m_reverseAPIDeviceIndex);
    response.getDaemonSourceSettings()->setReverseApiChannelIndex(settings.m_reverseAPIChannelIndex);
}

void DaemonSource::webapiFormatChannelReport(SWGSDRangel::SWGChannelReport& response)
{
    struct timeval tv;
    gettimeofday(&tv, 0);

    response.getDaemonSourceReport()->setTvSec(tv.tv_sec);
    response.getDaemonSourceReport()->setTvUSec(tv.tv_usec);
    response.getDaemonSourceReport()->setQueueSize(m_dataReadQueue.size());
    response.getDaemonSourceReport()->setQueueLength(m_dataReadQueue.length());
    response.getDaemonSourceReport()->setSamplesCount(m_dataReadQueue.readSampleCount());
    response.getDaemonSourceReport()->setCorrectableErrorsCount(m_nbCorrectableErrors);
    response.getDaemonSourceReport()->setUncorrectableErrorsCount(m_nbUncorrectableErrors);
    response.getDaemonSourceReport()->setNbOriginalBlocks(m_currentMeta.m_nbOriginalBlocks);
    response.getDaemonSourceReport()->setNbFecBlocks(m_currentMeta.m_nbFECBlocks);
    response.getDaemonSourceReport()->setCenterFreq(m_currentMeta.m_centerFrequency);
    response.getDaemonSourceReport()->setSampleRate(m_currentMeta.m_sampleRate);
    response.getDaemonSourceReport()->setDeviceCenterFreq(m_deviceAPI->getSampleSink()->getCenterFrequency()/1000);
    response.getDaemonSourceReport()->setDeviceSampleRate(m_deviceAPI->getSampleSink()->getSampleRate());
}

void DaemonSource::webapiReverseSendSettings(QList<QString>& channelSettingsKeys, const DaemonSourceSettings& settings, bool force)
{
    SWGSDRangel::SWGChannelSettings *swgChannelSettings = new SWGSDRangel::SWGChannelSettings();
    swgChannelSettings->setTx(1);
    swgChannelSettings->setChannelType(new QString("DaemonSource"));
    swgChannelSettings->setDaemonSourceSettings(new SWGSDRangel::SWGDaemonSourceSettings());
    SWGSDRangel::SWGDaemonSourceSettings *swgDaemonSourceSettings = swgChannelSettings->getDaemonSourceSettings();

    // transfer data that has been modified. When force is on transfer all data except reverse API data

    if (channelSettingsKeys.contains("dataAddress") || force) {
        swgDaemonSourceSettings->setDataAddress(new QString(settings.m_dataAddress));
    }
    if (channelSettingsKeys.contains("dataPort") || force) {
        swgDaemonSourceSettings->setDataPort(settings.m_dataPort);
    }
    if (channelSettingsKeys.contains("rgbColor") || force) {
        swgDaemonSourceSettings->setRgbColor(settings.m_rgbColor);
    }
    if (channelSettingsKeys.contains("title") || force) {
        swgDaemonSourceSettings->setTitle(new QString(settings.m_title));
    }

    QString channelSettingsURL = QString("http://%1:%2/sdrangel/deviceset/%3/channel/%4/settings")
            .arg(settings.m_reverseAPIAddress)
            .arg(settings.m_reverseAPIPort)
            .arg(settings.m_reverseAPIDeviceIndex)
            .arg(settings.m_reverseAPIChannelIndex);
    m_networkRequest.setUrl(QUrl(channelSettingsURL));
    m_networkRequest.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QBuffer *buffer=new QBuffer();
    buffer->open((QBuffer::ReadWrite));
    buffer->write(swgChannelSettings->asJson().toUtf8());
    buffer->seek(0);

    // Always use PATCH to avoid passing reverse API settings
    m_networkManager->sendCustomRequest(m_networkRequest, "PATCH", buffer);

    delete swgChannelSettings;
}

void DaemonSource::networkManagerFinished(QNetworkReply *reply)
{
    QNetworkReply::NetworkError replyError = reply->error();

    if (replyError)
    {
        qWarning() << "DaemonSource::networkManagerFinished:"
                << " error(" << (int) replyError
                << "): " << replyError
                << ": " << reply->errorString();
        return;
    }

    QString answer = reply->readAll();
    answer.chop(1); // remove last \n
    qDebug("DaemonSource::networkManagerFinished: reply:\n%s", answer.toStdString().c_str());
}
