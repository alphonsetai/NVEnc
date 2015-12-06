﻿//  -----------------------------------------------------------------------------------------
//    NVEnc by rigaya
//  -----------------------------------------------------------------------------------------
//   ソースコードについて
//   ・無保証です。
//   ・本ソースコードを使用したことによるいかなる損害・トラブルについてrigayaは責任を負いません。
//   以上に了解して頂ける場合、本ソースコードの使用、複製、改変、再頒布を行って頂いて構いません。
//  -----------------------------------------------------------------------------------------

#include "NVEncCore.h"
#include "CuvidDecode.h"
#include "helper_cuda.h"
#if ENABLE_AVCUVID_READER

bool check_if_nvcuvid_dll_available() {
    //check for nvcuvid.dll
    HMODULE hModule = LoadLibrary(_T("nvcuvid.dll"));
    if (hModule == NULL)
        return false;
    FreeLibrary(hModule);
    return true;
}


static int CUDAAPI HandleVideoData(void *pUserData, CUVIDSOURCEDATAPACKET *pPacket) {
    assert(pUserData);
    return ((CuvidDecode*)pUserData)->DecVideoData(pPacket);
}

static int CUDAAPI HandleVideoSequence(void *pUserData, CUVIDEOFORMAT *pFormat) {
    assert(pUserData);
    return ((CuvidDecode*)pUserData)->DecVideoSequence(pFormat);
}

static int CUDAAPI HandlePictureDecode(void *pUserData, CUVIDPICPARAMS *pPicParams) {
    assert(pUserData);
    return ((CuvidDecode*)pUserData)->DecPictureDecode(pPicParams);
}

static int CUDAAPI HandlePictureDisplay(void *pUserData, CUVIDPARSERDISPINFO *pPicParams) {
    assert(pUserData);
    return ((CuvidDecode*)pUserData)->DecPictureDisplay(pPicParams);
}

CuvidDecode::CuvidDecode() :
    m_pFrameQueue(nullptr), m_decodedFrames(0), m_videoParser(nullptr), m_videoDecoder(nullptr),
    m_ctxLock(nullptr), m_pPrintMes(), m_bIgnoreDynamicFormatChange(false), m_bError(false) {
    memset(&m_videoDecodeCreateInfo, 0, sizeof(m_videoDecodeCreateInfo));
    memset(&m_videoFormatEx, 0, sizeof(m_videoFormatEx));
}

CuvidDecode::~CuvidDecode() {
    CloseDecoder();
}

int CuvidDecode::DecVideoData(CUVIDSOURCEDATAPACKET *pPacket) {
    CUresult curesult = CUDA_SUCCESS;
    //cuvidCtxLock(m_ctxLock, 0);
    __try {
        curesult = cuvidParseVideoData(m_videoParser, pPacket);
    } __except(1) {
        AddMessage(NV_LOG_ERROR, _T("cuvidParseVideoData error\n"));
        curesult = CUDA_ERROR_UNKNOWN;
    }
    //cuvidCtxUnlock(m_ctxLock, 0);
    if (curesult != CUDA_SUCCESS) {
        m_bError = true;
    }
    return (curesult == CUDA_SUCCESS);
}

int CuvidDecode::DecPictureDecode(CUVIDPICPARAMS *pPicParams) {
    AddMessage(NV_LOG_TRACE, _T("DecPictureDecode idx: %d\n"), pPicParams->CurrPicIdx);
    m_pFrameQueue->waitUntilFrameAvailable(pPicParams->CurrPicIdx);
    CUresult curesult = CUDA_SUCCESS;
    //cuvidCtxLock(m_ctxLock, 0);
    __try {
        curesult = cuvidDecodePicture(m_videoDecoder, pPicParams);
    } __except(1) {
        AddMessage(NV_LOG_ERROR, _T("cuvidDecodePicture error\n"));
        curesult = CUDA_ERROR_UNKNOWN;
    }
    //cuvidCtxUnlock(m_ctxLock, 0);
    if (curesult != CUDA_SUCCESS) {
        m_bError = true;
    }
    return (curesult == CUDA_SUCCESS);
}

int CuvidDecode::DecVideoSequence(CUVIDEOFORMAT *pFormat) {
    AddMessage(NV_LOG_TRACE, _T("DecVideoSequence\n"));
    if (   (pFormat->codec         != m_videoDecodeCreateInfo.CodecType)
        || (pFormat->coded_width   != m_videoDecodeCreateInfo.ulWidth)
        || (pFormat->coded_height  != m_videoDecodeCreateInfo.ulHeight)
        || (pFormat->chroma_format != m_videoDecodeCreateInfo.ChromaFormat)) {
        AddMessage((m_bIgnoreDynamicFormatChange) ? NV_LOG_DEBUG : NV_LOG_ERROR, _T("dynamic video format changing detected\n"));
        m_videoDecodeCreateInfo.CodecType    = pFormat->codec;
        m_videoDecodeCreateInfo.ulWidth      = pFormat->coded_width;
        m_videoDecodeCreateInfo.ulHeight     = pFormat->coded_height;
        m_videoDecodeCreateInfo.ChromaFormat = pFormat->chroma_format;
        if (!m_bIgnoreDynamicFormatChange) {
            m_bError = true;
        }
        return 0;
    }
    return 1;
}

int CuvidDecode::DecPictureDisplay(CUVIDPARSERDISPINFO *pPicParams) {
    AddMessage(NV_LOG_TRACE, _T("DecPictureDisplay idx: %d, %I64d\n"), pPicParams->picture_index, pPicParams->timestamp);
    m_pFrameQueue->enqueue(pPicParams);
    m_decodedFrames++;

    return 1;
}

void CuvidDecode::CloseDecoder() {
    if (m_videoDecoder) {
        cuvidDestroyDecoder(m_videoDecoder);
        m_videoDecoder = nullptr;
    }
    if (m_videoParser) {
        cuvidDestroyVideoParser(m_videoParser);
        m_videoParser = nullptr;
    }
    m_ctxLock = nullptr;
    m_pPrintMes.reset();
    if (m_pFrameQueue) {
        delete m_pFrameQueue;
        m_pFrameQueue = nullptr;
    }
    m_decodedFrames = 0;
    m_bError = false;
}

CUresult CuvidDecode::CreateDecoder() {
    CUresult curesult = CUDA_SUCCESS;
    __try {
        curesult = cuvidCreateDecoder(&m_videoDecoder, &m_videoDecodeCreateInfo);
    } __except (1) {
        AddMessage(NV_LOG_ERROR, _T("cuvidCreateDecoder error\n"));
        curesult = CUDA_ERROR_UNKNOWN;
    }
    return curesult;
}

CUresult CuvidDecode::InitDecode(CUvideoctxlock ctxLock, const InputVideoInfo *input, shared_ptr<CNVEncLog> pLog, bool ignoreDynamicFormatChange) {
    //初期化
    CloseDecoder();

    m_pPrintMes = pLog;
    m_bIgnoreDynamicFormatChange = ignoreDynamicFormatChange;

    if (!check_if_nvcuvid_dll_available()) {
        AddMessage(NV_LOG_ERROR, _T("nvcuvid.dll does not exist.\n"));
        return CUDA_ERROR_NOT_FOUND;
    }
    AddMessage(NV_LOG_DEBUG, _T("nvcuvid.dll available\n"));

    m_ctxLock = ctxLock;

    if (nullptr == (m_pFrameQueue = new CUVIDFrameQueue(m_ctxLock))) {
        AddMessage(NV_LOG_ERROR, _T("Failed to alloc frame queue for decoder.\n"));
        return CUDA_ERROR_OUT_OF_MEMORY;
    }
    m_pFrameQueue->init(input->width, input->height);
    AddMessage(NV_LOG_DEBUG, _T("created frame queue\n"));

    //init video parser
    memset(&m_videoFormatEx, 0, sizeof(CUVIDEOFORMATEX));
    if (input->codecExtra && input->codecExtraSize) {
        memcpy(m_videoFormatEx.raw_seqhdr_data, input->codecExtra, input->codecExtraSize);
        m_videoFormatEx.format.seqhdr_data_length = input->codecExtraSize;
    }

    CUVIDPARSERPARAMS oVideoParserParameters;
    memset(&oVideoParserParameters, 0, sizeof(CUVIDPARSERPARAMS));
    oVideoParserParameters.CodecType              = input->codec;
    oVideoParserParameters.ulMaxNumDecodeSurfaces = FrameQueue::cnMaximumSize;
    oVideoParserParameters.ulMaxDisplayDelay      = 1;
    oVideoParserParameters.pUserData              = this;
    oVideoParserParameters.pfnSequenceCallback    = HandleVideoSequence;
    oVideoParserParameters.pfnDecodePicture       = HandlePictureDecode;
    oVideoParserParameters.pfnDisplayPicture      = HandlePictureDisplay;
    oVideoParserParameters.pExtVideoInfo          = &m_videoFormatEx;

    CUresult curesult = CUDA_SUCCESS;
    if (CUDA_SUCCESS != (curesult = cuvidCreateVideoParser(&m_videoParser, &oVideoParserParameters))) {
        AddMessage(NV_LOG_ERROR, _T("Failed cuvidCreateVideoParser %d (%s)\n"), curesult, char_to_tstring(_cudaGetErrorEnum(curesult)).c_str());
        return curesult;
    }
    AddMessage(NV_LOG_DEBUG, _T("created video parser\n"));

    cuvidCtxLock(m_ctxLock, 0);
    memset(&m_videoDecodeCreateInfo, 0, sizeof(CUVIDDECODECREATEINFO));
    m_videoDecodeCreateInfo.CodecType = input->codec;
    m_videoDecodeCreateInfo.ulWidth   = input->codedWidth  ? input->codedWidth  : input->width;
    m_videoDecodeCreateInfo.ulHeight  = input->codedHeight ? input->codedHeight : input->height;
    m_videoDecodeCreateInfo.ulNumDecodeSurfaces = FrameQueue::cnMaximumSize;

    m_videoDecodeCreateInfo.ChromaFormat = cudaVideoChromaFormat_420;
    m_videoDecodeCreateInfo.OutputFormat = cudaVideoSurfaceFormat_NV12;
    //m_videoDecodeCreateInfo.DeinterlaceMode = cudaVideoDeinterlaceMode_Adaptive;

    if (input->dstWidth > 0 && input->dstHeight > 0) {
        m_videoDecodeCreateInfo.ulTargetWidth  = input->dstWidth;
        m_videoDecodeCreateInfo.ulTargetHeight = input->dstHeight;
    } else {
        m_videoDecodeCreateInfo.ulTargetWidth  = input->width;
        m_videoDecodeCreateInfo.ulTargetHeight = input->height;
    }

    m_videoDecodeCreateInfo.display_area.left   = (short)input->crop.e.left;
    m_videoDecodeCreateInfo.display_area.top    = (short)input->crop.e.up;
    m_videoDecodeCreateInfo.display_area.right  = (short)(input->crop.e.right  + input->codedWidth  - input->width);
    m_videoDecodeCreateInfo.display_area.bottom = (short)(input->crop.e.bottom + input->codedHeight - input->height);

    m_videoDecodeCreateInfo.ulNumOutputSurfaces = 1;
    m_videoDecodeCreateInfo.ulCreationFlags = cudaVideoCreate_PreferCUVID;
    m_videoDecodeCreateInfo.vidLock = m_ctxLock;
    curesult = CreateDecoder();
    cuvidCtxUnlock(m_ctxLock, 0);
    if (CUDA_SUCCESS != curesult) {
        AddMessage(NV_LOG_ERROR, _T("Failed cuvidCreateDecoder %d (%s)\n"), curesult, char_to_tstring(_cudaGetErrorEnum(curesult)).c_str());
        return curesult;
    }
    AddMessage(NV_LOG_DEBUG, _T("created decoder\n"));

    if (m_videoFormatEx.raw_seqhdr_data && m_videoFormatEx.format.seqhdr_data_length) {
        if (CUDA_SUCCESS != (curesult = DecodePacket(m_videoFormatEx.raw_seqhdr_data, m_videoFormatEx.format.seqhdr_data_length, AV_NOPTS_VALUE, CUVID_NATIVE_TIMEBASE))) {
            AddMessage(NV_LOG_ERROR, _T("Failed to decode header %d (%s).\n"), curesult, char_to_tstring(_cudaGetErrorEnum(curesult)).c_str());
            return curesult;
        }
    }

    return curesult;
}

CUresult CuvidDecode::FlushParser() {
    CUVIDSOURCEDATAPACKET pCuvidPacket;
    memset(&pCuvidPacket, 0, sizeof(pCuvidPacket));

    pCuvidPacket.flags |= CUVID_PKT_ENDOFSTREAM;
    CUresult result = CUDA_SUCCESS;

    //cuvidCtxLock(m_ctxLock, 0);
    __try {
        result = cuvidParseVideoData(m_videoParser, &pCuvidPacket);
    } __except (1) {
        AddMessage(NV_LOG_ERROR, _T("cuvidParseVideoData error\n"));
        result = CUDA_ERROR_UNKNOWN;
    }
    //cuvidCtxUnlock(m_ctxLock, 0);
    m_pFrameQueue->endDecode();
    return result;
}

CUresult CuvidDecode::DecodePacket(uint8_t *data, size_t nSize, int64_t timestamp, AVRational streamtimebase) {
    if (data == nullptr || nSize == 0) {
        return FlushParser();
    }

    CUVIDSOURCEDATAPACKET pCuvidPacket;
    memset(&pCuvidPacket, 0, sizeof(pCuvidPacket));
    pCuvidPacket.payload      = data;
    pCuvidPacket.payload_size = (uint32_t)nSize;
    CUresult result = CUDA_SUCCESS;

    if (timestamp != AV_NOPTS_VALUE) {
        pCuvidPacket.flags     |= CUVID_PKT_TIMESTAMP;
        pCuvidPacket.timestamp  = av_rescale_q(timestamp, streamtimebase, CUVID_NATIVE_TIMEBASE);
    }

    //cuvidCtxLock(m_ctxLock, 0);
    __try {
        result = cuvidParseVideoData(m_videoParser, &pCuvidPacket);
    } __except (1) {
        AddMessage(NV_LOG_ERROR, _T("cuvidParseVideoData error\n"));
        result = CUDA_ERROR_UNKNOWN;
    }
    //cuvidCtxUnlock(m_ctxLock, 0);
    return result;
}

#endif // #if ENABLE_AVCUVID_READER