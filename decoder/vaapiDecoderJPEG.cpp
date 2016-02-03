/*
 * Copyright 2016 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

// primary header
#include "vaapiDecoderJPEG.h"

// library headers
#include "codecparsers/jpegParser.h"
#include "common/common_def.h"
#include "vaapidecoder_factory.h"

// system headers
#include <cassert>
#include <tr1/array>
#include <tr1/functional>
#include <tr1/memory>

using ::YamiParser::JPEG::Component;
using ::YamiParser::JPEG::FrameHeader;
using ::YamiParser::JPEG::HuffTable;
using ::YamiParser::JPEG::HuffTables;
using ::YamiParser::JPEG::Parser;
using ::YamiParser::JPEG::QuantTable;
using ::YamiParser::JPEG::QuantTables;
using ::YamiParser::JPEG::ScanHeader;
using ::YamiParser::JPEG::Defaults;
using ::std::tr1::function;
using ::std::tr1::bind;
using ::std::tr1::ref;

namespace YamiMediaCodec {

struct Slice {
    Slice() : data(NULL), start(0) , length(0) { }

    const uint8_t* data;
    uint32_t start;
    uint32_t length;
};

class VaapiDecoderJPEG::Impl
{
public:
    typedef function<Decode_Status (void)> DecodeHandler;

    Impl(const DecodeHandler& start, const DecodeHandler& finish)
        : m_startHandler(start)
        , m_finishHandler(finish)
        , m_parser()
        , m_dcHuffmanTables(Defaults::instance().dcHuffTables())
        , m_acHuffmanTables(Defaults::instance().acHuffTables())
        , m_quantizationTables(Defaults::instance().quantTables())
        , m_slice()
        , m_decodeStatus(DECODE_SUCCESS)
    {
    }

    Decode_Status decode(const uint8_t* data, const uint32_t size)
    {
        using namespace ::YamiParser::JPEG;

        if (!data || !size)
            return DECODE_FAIL;

        /*
         * Reset the parser if we have a new data pointer; this is common for
         * MJPEG. If the data pointer is the same, then the assumption is that
         * we are continuing after previously suspending due to an SOF
         * DECODE_FORMAT_CHANGE.
         */
        if (m_slice.data != data)
            m_parser.reset();

        if (!m_parser) { /* First call or new data */
            Parser::Callback defaultCallback =
                bind(&Impl::onMarker, ref(*this));
            Parser::Callback sofCallback =
                bind(&Impl::onStartOfFrame, ref(*this));
            m_slice.data = data;
            m_parser.reset(new Parser(data, size));
            m_parser->registerCallback(M_SOI, defaultCallback);
            m_parser->registerCallback(M_EOI, defaultCallback);
            m_parser->registerCallback(M_SOS, defaultCallback);
            m_parser->registerCallback(M_DHT, defaultCallback);
            m_parser->registerCallback(M_DQT, defaultCallback);
            m_parser->registerStartOfFrameCallback(sofCallback);
        }

        if (!m_parser->parse())
            m_decodeStatus = DECODE_FAIL;

        return m_decodeStatus;
    }

    const FrameHeader::Shared& frameHeader() const
    {
        return m_parser->frameHeader();
    }

    const ScanHeader::Shared& scanHeader() const
    {
        return m_parser->scanHeader();
    }

    const unsigned restartInterval() const
    {
        return m_parser->restartInterval();
    }

    const HuffTables& dcHuffmanTables() const { return m_dcHuffmanTables; }
    const HuffTables& acHuffmanTables() const { return m_acHuffmanTables; }
    const QuantTables& quantTables() const { return m_quantizationTables; }
    const Slice& slice() const { return m_slice; }

private:
    Parser::CallbackResult onMarker()
    {
        using namespace ::YamiParser::JPEG;

        m_decodeStatus = DECODE_SUCCESS;

        switch(m_parser->current().marker) {
        case M_SOI:
            m_slice.start = 0;
            m_slice.length = 0;
            break;
        case M_SOS:
            m_slice.start = m_parser->current().position + 1
                + m_parser->current().length;
            break;
        case M_EOI:
            m_slice.length = m_parser->current().position - m_slice.start;
            m_decodeStatus = m_finishHandler();
            break;
        case M_DQT:
            m_quantizationTables = m_parser->quantTables();
            break;
        case M_DHT:
            m_dcHuffmanTables = m_parser->dcHuffTables();
            m_acHuffmanTables = m_parser->acHuffTables();
            break;
        default:
            m_decodeStatus = DECODE_FAIL;
        }

        if (m_decodeStatus != DECODE_SUCCESS)
            return Parser::ParseSuspend;
        return Parser::ParseContinue;
    }

    Parser::CallbackResult onStartOfFrame()
    {
        m_decodeStatus = m_startHandler();
        if (m_decodeStatus != DECODE_SUCCESS)
            return Parser::ParseSuspend;
        return Parser::ParseContinue;
    }

    const DecodeHandler m_startHandler; // called after SOF
    const DecodeHandler m_finishHandler; // called after EOI

    Parser::Shared m_parser;
    HuffTables m_dcHuffmanTables;
    HuffTables m_acHuffmanTables;
    QuantTables m_quantizationTables;

    Slice m_slice;

    Decode_Status m_decodeStatus;
};

VaapiDecoderJPEG::VaapiDecoderJPEG()
    : VaapiDecoderBase::VaapiDecoderBase()
    , m_impl()
    , m_picture()
{
    return;
}

Decode_Status VaapiDecoderJPEG::fillPictureParam()
{
    const FrameHeader::Shared frame = m_impl->frameHeader();

    const size_t numComponents = frame->components.size();

    if (numComponents > 4)
        return DECODE_FAIL;

    VAPictureParameterBufferJPEGBaseline* vaPicParam(NULL);

    if (!m_picture->editPicture(vaPicParam))
        return DECODE_FAIL;

    for (size_t i(0); i < numComponents; ++i) {
        const Component::Shared& component = frame->components[i];
        vaPicParam->components[i].component_id = component->id;
        vaPicParam->components[i].h_sampling_factor = component->hSampleFactor;
        vaPicParam->components[i].v_sampling_factor = component->vSampleFactor;
        vaPicParam->components[i].quantiser_table_selector =
            component->quantTableNumber;
    }

    vaPicParam->picture_width = frame->imageWidth;
    vaPicParam->picture_height = frame->imageHeight;
    vaPicParam->num_components = frame->components.size();

    return DECODE_SUCCESS;
}

Decode_Status VaapiDecoderJPEG::fillSliceParam()
{
    const ScanHeader::Shared scan = m_impl->scanHeader();
    const FrameHeader::Shared frame = m_impl->frameHeader();
    const Slice& slice = m_impl->slice();
    VASliceParameterBufferJPEGBaseline *sliceParam(NULL);

    if (!m_picture->newSlice(sliceParam, slice.data + slice.start, slice.length))
        return DECODE_FAIL;

    for (size_t i(0); i < scan->numComponents; ++i) {
        sliceParam->components[i].component_selector =
            scan->components[i]->id;
        sliceParam->components[i].dc_table_selector =
            scan->components[i]->dcTableNumber;
        sliceParam->components[i].ac_table_selector =
            scan->components[i]->acTableNumber;
    }

    sliceParam->restart_interval = m_impl->restartInterval();
    sliceParam->num_components = scan->numComponents;
    sliceParam->slice_horizontal_position = 0;
    sliceParam->slice_vertical_position = 0;

    int width = frame->imageWidth;
    int height = frame->imageHeight;
    int maxHSample = frame->maxHSampleFactor << 3;
    int maxVSample = frame->maxVSampleFactor << 3;
    int codedWidth;
    int codedHeight;

    if (scan->numComponents == 1) { /* Noninterleaved Scan */
        if (frame->components.front() == scan->components.front()) {
            /* Y mcu */
            codedWidth = width >> 3;
            codedHeight = height >> 3;
        } else {
            /* Cr, Cb mcu */
            codedWidth = width >> 4;
            codedHeight = height >> 4;
        }
    } else { /* Interleaved Scan */
        codedWidth = (width + maxHSample - 1) / maxHSample;
        codedHeight = (height + maxVSample - 1) / maxVSample;
    }

    sliceParam->num_mcus = codedWidth * codedHeight;

    return DECODE_SUCCESS;
}

Decode_Status VaapiDecoderJPEG::loadQuantizationTables()
{
    using namespace ::YamiParser::JPEG;

    VAIQMatrixBufferJPEGBaseline* vaIqMatrix(NULL);

    if (!m_picture->editIqMatrix(vaIqMatrix))
        return DECODE_FAIL;

    size_t numTables = std::min(
        N_ELEMENTS(vaIqMatrix->quantiser_table), size_t(NUM_QUANT_TBLS));

    for (size_t i(0); i < numTables; ++i) {
        const QuantTable::Shared& quantTable = m_impl->quantTables()[i];
        vaIqMatrix->load_quantiser_table[i] = bool(quantTable);
        if (!quantTable)
            continue;
        assert(quantTable->precision == 0);
        for (uint32_t j(0); j < DCTSIZE2; ++j)
            vaIqMatrix->quantiser_table[i][j] = quantTable->values[j];
    }

    return DECODE_SUCCESS;
}

Decode_Status VaapiDecoderJPEG::loadHuffmanTables()
{
    using namespace ::YamiParser::JPEG;

    VAHuffmanTableBufferJPEGBaseline* vaHuffmanTable(NULL);

    if (!m_picture->editHufTable(vaHuffmanTable))
        return DECODE_FAIL;

    size_t numTables = std::min(
        N_ELEMENTS(vaHuffmanTable->huffman_table), size_t(NUM_HUFF_TBLS));

    for (size_t i(0); i < numTables; ++i) {
        const HuffTable::Shared& dcTable = m_impl->dcHuffmanTables()[i];
        const HuffTable::Shared& acTable = m_impl->acHuffmanTables()[i];
        bool valid = bool(dcTable) && bool(acTable);
        vaHuffmanTable->load_huffman_table[i] = valid;
        if (!valid)
            continue;

        // Load DC Table
        memcpy(vaHuffmanTable->huffman_table[i].num_dc_codes,
            &dcTable->codes[0],
            sizeof(vaHuffmanTable->huffman_table[i].num_dc_codes));
        memcpy(vaHuffmanTable->huffman_table[i].dc_values,
            &dcTable->values[0],
            sizeof(vaHuffmanTable->huffman_table[i].dc_values));

        // Load AC Table
        memcpy(vaHuffmanTable->huffman_table[i].num_ac_codes,
            &acTable->codes[0],
            sizeof(vaHuffmanTable->huffman_table[i].num_ac_codes));
        memcpy(vaHuffmanTable->huffman_table[i].ac_values,
            &acTable->values[0],
            sizeof(vaHuffmanTable->huffman_table[i].ac_values));

        memset(vaHuffmanTable->huffman_table[i].pad,
                0, sizeof(vaHuffmanTable->huffman_table[i].pad));
    }

    return DECODE_SUCCESS;
}

Decode_Status VaapiDecoderJPEG::decode(VideoDecodeBuffer* buffer)
{
    if (!buffer)
        return DECODE_FAIL;

    m_currentPTS = buffer->timeStamp;

    if (!m_impl.get())
        m_impl.reset(new VaapiDecoderJPEG::Impl(
            bind(&VaapiDecoderJPEG::start, ref(*this), &m_configBuffer),
            bind(&VaapiDecoderJPEG::finish, ref(*this))));

    return m_impl->decode(buffer->data, buffer->size);
}

Decode_Status VaapiDecoderJPEG::start(VideoConfigBuffer* buffer)
{
    DEBUG("%s", __func__);

    m_configBuffer = *buffer;
    m_configBuffer.surfaceNumber = 2;
    m_configBuffer.profile = VAProfileJPEGBaseline;

    /* We can't start until decoding has started */
    if (!m_impl.get())
        return DECODE_SUCCESS;

    const FrameHeader::Shared frame = m_impl->frameHeader();

    /*
     * We don't expect the user to call start() after a failed decode() attempt.
     * But since the user can, we must check if we have a valid frame header
     * before we can use it.  That is, m_parser could be initialized but the
     * frame header might not be.
     */
    if (!frame)
        return DECODE_FAIL;

    if (!frame->isBaseline) {
        ERROR("Unsupported JPEG profile. Only JPEG Baseline is supported.");
        return DECODE_FAIL;
    }

    m_configBuffer.width = frame->imageWidth;
    m_configBuffer.height = frame->imageHeight;
    m_configBuffer.surfaceWidth = frame->imageWidth;
    m_configBuffer.surfaceHeight = frame->imageHeight;

    /* Now we can actually start */
    if (!VaapiDecoderBase::start(&m_configBuffer))
        return DECODE_FAIL;

    return DECODE_FORMAT_CHANGE;
}

Decode_Status VaapiDecoderJPEG::finish()
{
    if (!m_impl->frameHeader()) {
        ERROR("Start of Frame (SOF) not found");
        return DECODE_FAIL;
    }

    if (!m_impl->scanHeader()) {
        ERROR("Start of Scan (SOS) not found");
        return DECODE_FAIL;
    }

    m_picture = createPicture(m_currentPTS);
    if (!m_picture) {
        ERROR("Could not create a VAAPI picture.");
        return DECODE_FAIL;
    }

    m_picture->m_timeStamp = m_currentPTS;

    if (!fillSliceParam()) {
        ERROR("Failed to load VAAPI slice parameters.");
        return DECODE_FAIL;
    }

    if (!fillPictureParam()) {
        ERROR("Failed to load VAAPI picture parameters");
        return DECODE_FAIL;
    }

    if (!loadQuantizationTables()) {
        ERROR("Failed to load VAAPI quantization tables");
        return DECODE_FAIL;
    }

    if (!loadHuffmanTables()) {
        ERROR("Failed to load VAAPI huffman tables");
        return DECODE_FAIL;
    }

    if (!m_picture->decode())
        return DECODE_FAIL;

    if (!outputPicture(m_picture))
        return DECODE_FAIL;

    return DECODE_SUCCESS;
}

Decode_Status VaapiDecoderJPEG::reset(VideoConfigBuffer * buffer)
{
    DEBUG("%s", __func__);

    m_picture.reset();

    m_impl.reset();

    return VaapiDecoderBase::reset(buffer);
}

const bool VaapiDecoderJPEG::s_registered =
    VaapiDecoderFactory::register_<VaapiDecoderJPEG>(YAMI_MIME_JPEG);
}