/*******************************************************************************
 * Project:  Nebula
 * @file     SocketChannelImpl.cpp
 * @brief 
 * @author   Bwar
 * @date:    2016年8月10日
 * @note
 * Modify history:
 ******************************************************************************/
#include <cstring>
#include "labor/WorkerImpl.hpp"
#include "codec/CodecProto.hpp"
#include "codec/CodecPrivate.hpp"
#include "codec/CodecHttp.hpp"
#include "labor/Labor.hpp"
#include "labor/Manager.hpp"
#include "util/logger/NetLogger.hpp"
#include "SocketChannelImpl.hpp"

namespace neb
{

SocketChannelImpl::SocketChannelImpl(SocketChannel* pSocketChannel, std::shared_ptr<NetLogger> pLogger, int iFd, uint32 ulSeq, ev_tstamp dKeepAlive)
    : m_ucChannelStatus(CHANNEL_STATUS_INIT), m_unRemoteWorkerIdx(0), m_iFd(iFd), m_ulSeq(ulSeq), m_ulForeignSeq(0), m_ulStepSeq(0),
      m_ulUnitTimeMsgNum(0), m_ulMsgNum(0), m_dActiveTime(0.0), m_dKeepAlive(dKeepAlive),
      m_pIoWatcher(nullptr), m_pTimerWatcher(nullptr),
      m_pRecvBuff(nullptr), m_pSendBuff(nullptr), m_pWaitForSendBuff(nullptr),
      m_pCodec(nullptr), m_iErrno(0), m_pLabor(nullptr), m_pSocketChannel(pSocketChannel), m_pLogger(pLogger)
{
    memset(m_szErrBuff, 0, sizeof(m_szErrBuff));
}

SocketChannelImpl::~SocketChannelImpl()
{
    Abort();
    FREE(m_pIoWatcher);
    FREE(m_pTimerWatcher);
    DELETE(m_pRecvBuff);
    DELETE(m_pSendBuff);
    DELETE(m_pWaitForSendBuff);
    DELETE(m_pCodec);
    LOG4_DEBUG("SocketChannelImpl::~SocketChannelImpl() fd %d, seq %u", m_iFd, m_ulSeq);
}

bool SocketChannelImpl::Init(E_CODEC_TYPE eCodecType, bool bIsServer, const std::string& strKey)
{
    LOG4_TRACE("fd[%d], codec_type[%d]", m_iFd, eCodecType);
    try
    {
        m_pRecvBuff = new CBuffer();
        m_pSendBuff = new CBuffer();
        m_pWaitForSendBuff = new CBuffer();
        switch (eCodecType)
        {
            case CODEC_NEBULA:
                m_pCodec = new CodecProto(m_pLogger, eCodecType, strKey);
                m_ucChannelStatus = CHANNEL_STATUS_INIT;
                break;
            case CODEC_PRIVATE:
                m_pCodec = new CodecPrivate(m_pLogger, eCodecType, strKey);
                m_ucChannelStatus = CHANNEL_STATUS_ESTABLISHED;
                break;
            case CODEC_HTTP:
                m_pCodec = new CodecHttp(m_pLogger, eCodecType, strKey);
                m_ucChannelStatus = CHANNEL_STATUS_ESTABLISHED;
                break;
            default:
                LOG4_ERROR("no codec defined for code type %d", eCodecType);
                break;
        }
    }
    catch(std::bad_alloc& e)
    {
        LOG4_ERROR("%s", e.what());
        return(false);
    }
    m_strKey = strKey;
    m_dActiveTime = m_pLabor->GetNowTime();
    return(true);
}

E_CODEC_TYPE SocketChannelImpl::GetCodecType() const
{
    return(m_pCodec->GetCodecType());
}

ev_tstamp SocketChannelImpl::GetKeepAlive()
{
    if (CODEC_HTTP == m_pCodec->GetCodecType())
    {
        m_dKeepAlive = (((CodecHttp*)m_pCodec)->GetKeepAlive() >= 0.0) ? ((CodecHttp*)m_pCodec)->GetKeepAlive() : m_dKeepAlive;
    }
    return(m_dKeepAlive);
}

bool SocketChannelImpl::NeedAliveCheck() const
{
    if (CODEC_HTTP == m_pCodec->GetCodecType())
    {
        return(false);
    }
    return(true);
}

E_CODEC_STATUS SocketChannelImpl::Send()
{
    LOG4_TRACE("channel_fd[%d], channel_seq[%d], channel_status[%d]", m_iFd, m_ulSeq, m_ucChannelStatus);
    if (CHANNEL_STATUS_ABORT == m_ucChannelStatus)
    {
        LOG4_WARNING("channel_fd[%d], channel_seq[%d], channel_status[%d] send EOF.", m_iFd, m_ulSeq, m_ucChannelStatus);
        return(CODEC_STATUS_EOF);
    }
    else if (CHANNEL_STATUS_ESTABLISHED != m_ucChannelStatus)
    {
        return(CODEC_STATUS_PAUSE);
    }
    int iNeedWriteLen = 0;
    int iWriteLen = 0;
    iNeedWriteLen = m_pSendBuff->ReadableBytes();
    if (0 == iNeedWriteLen)
    {
        iNeedWriteLen = m_pWaitForSendBuff->ReadableBytes();
        if (0 == iNeedWriteLen)
        {
            if (CODEC_NEBULA != m_pCodec->GetCodecType() && m_dKeepAlive <= 0.0)
            {
                return(CODEC_STATUS_EOF);
            }
            else
            {
                LOG4_DEBUG("no data need to send.");
                return(CODEC_STATUS_OK);
            }
        }
        else
        {
            CBuffer* pExchangeBuff = m_pSendBuff;
            m_pSendBuff = m_pWaitForSendBuff;
            m_pWaitForSendBuff = pExchangeBuff;
            m_pWaitForSendBuff->Compact(1);
        }
    }

    m_dActiveTime = m_pLabor->GetNowTime();
    iWriteLen = Write(m_pSendBuff, m_iErrno);
    LOG4_TRACE("iNeedWriteLen = %d, iWriteLen = %d", iNeedWriteLen, iWriteLen);
    if (iWriteLen >= 0)
    {
        m_dActiveTime = m_pLabor->GetNowTime();
        if (iNeedWriteLen == iWriteLen && 0 == m_pWaitForSendBuff->ReadableBytes())
        {
            return(CODEC_STATUS_OK);
        }
        else
        {
            return(CODEC_STATUS_PAUSE);
        }
    }
    else
    {
        if (EAGAIN == m_iErrno && EINTR == m_iErrno)    // 对非阻塞socket而言，EAGAIN不是一种错误;EINTR即errno为4，错误描述Interrupted system call，操作也应该继续。
        {
            m_dActiveTime = m_pLabor->GetNowTime();
            return(CODEC_STATUS_PAUSE);
        }
        LOG4_ERROR("send to fd %d error %d: %s",
                m_iFd, m_iErrno, strerror_r(m_iErrno, m_szErrBuff, sizeof(m_szErrBuff)));
        m_strErrMsg = m_szErrBuff;
        return(CODEC_STATUS_INT);
    }
}

E_CODEC_STATUS SocketChannelImpl::Send(int32 iCmd, uint32 uiSeq, const MsgBody& oMsgBody)
{
    LOG4_TRACE("channel_fd[%d], channel_seq[%d], cmd[%u], seq[%u]", m_iFd, m_ulSeq, iCmd, uiSeq);
    if (CHANNEL_STATUS_ABORT == m_ucChannelStatus)
    {
        LOG4_WARNING("channel_fd[%d], channel_seq[%d], channel_status[%d] send EOF.", m_iFd, m_ulSeq, m_ucChannelStatus);
        return(CODEC_STATUS_EOF);
    }
    E_CODEC_STATUS eCodecStatus = CODEC_STATUS_OK;
    int32 iMsgBodyLen = oMsgBody.ByteSize();
    MsgHead oMsgHead;
    oMsgHead.set_cmd(iCmd);
    oMsgHead.set_seq(uiSeq);
    iMsgBodyLen = (iMsgBodyLen > 0) ? iMsgBodyLen : -1;     // proto3里int赋值为0会在指定固定大小的message时有问题
    oMsgHead.set_len(iMsgBodyLen);
    switch (m_ucChannelStatus)
    {
        case CHANNEL_STATUS_ESTABLISHED:
            eCodecStatus = m_pCodec->Encode(oMsgHead, oMsgBody, m_pSendBuff);
            break;
        case CHANNEL_STATUS_TELL_WORKER:
        case CHANNEL_STATUS_WORKER:
        case CHANNEL_STATUS_TRANSFER_TO_WORKER:
        case CHANNEL_STATUS_CONNECTED:
        case CHANNEL_STATUS_TRY_CONNECT:
        case CHANNEL_STATUS_INIT:
        {
            switch (iCmd)
            {
                case CMD_RSP_TELL_WORKER:
                    m_ucChannelStatus = CHANNEL_STATUS_ESTABLISHED;
                    eCodecStatus = m_pCodec->Encode(oMsgHead, oMsgBody, m_pSendBuff);
                    break;
                case CMD_REQ_TELL_WORKER:
                    m_ucChannelStatus = CHANNEL_STATUS_TELL_WORKER;
                    eCodecStatus = m_pCodec->Encode(oMsgHead, oMsgBody, m_pSendBuff);
                    break;
                case CMD_RSP_CONNECT_TO_WORKER:
                    m_ucChannelStatus = CHANNEL_STATUS_WORKER;
                    eCodecStatus = m_pCodec->Encode(oMsgHead, oMsgBody, m_pSendBuff);
                    break;
                case CMD_REQ_CONNECT_TO_WORKER:
                    m_ucChannelStatus = CHANNEL_STATUS_TRANSFER_TO_WORKER;
                    eCodecStatus = m_pCodec->Encode(oMsgHead, oMsgBody, m_pSendBuff);
                    break;
                default:
                    eCodecStatus = m_pCodec->Encode(oMsgHead, oMsgBody, m_pWaitForSendBuff);
                    if (CODEC_STATUS_OK == eCodecStatus)
                    {
                        eCodecStatus = CODEC_STATUS_PAUSE;
                    }
                    break;
            }
        }
            break;
        default:
            LOG4_ERROR("invalid connect status %d!", m_ucChannelStatus);
            return(CODEC_STATUS_OK);
    }

    if (CODEC_STATUS_OK != eCodecStatus)
    {
        return(eCodecStatus);
    }

    int iNeedWriteLen = m_pSendBuff->ReadableBytes();
    if (iNeedWriteLen <= 0)
    {
        return(eCodecStatus);
    }

    errno = 0;
    int iWriteLen = Write(m_pSendBuff, m_iErrno);
    LOG4_TRACE("iNeedWriteLen = %d, iWriteLen = %d", iNeedWriteLen, iWriteLen);
    if (iWriteLen >= 0)
    {
        m_dActiveTime = m_pLabor->GetNowTime();
        if (iNeedWriteLen == iWriteLen)
        {
            if (CMD_RSP_TELL_WORKER == iCmd)
            {
                return(Send());
            }
            else
            {
                return(CODEC_STATUS_OK);
            }
        }
        else
        {
            return(CODEC_STATUS_PAUSE);
        }
    }
    else
    {
        if (EAGAIN == m_iErrno && EINTR == m_iErrno)    // 对非阻塞socket而言，EAGAIN不是一种错误;EINTR即errno为4，错误描述Interrupted system call，操作也应该继续。
        {
            m_dActiveTime = m_pLabor->GetNowTime();
            return(CODEC_STATUS_PAUSE);
        }
        LOG4_ERROR("send to fd %d error %d: %s",
                m_iFd, m_iErrno, strerror_r(m_iErrno, m_szErrBuff, sizeof(m_szErrBuff)));
        m_strErrMsg = m_szErrBuff;
        return(CODEC_STATUS_INT);
    }
}

E_CODEC_STATUS SocketChannelImpl::Send(const HttpMsg& oHttpMsg, uint32 ulStepSeq)
{
    LOG4_TRACE("channel_fd[%d], channel_seq[%d], channel_status[%d]", m_iFd, m_ulSeq, m_ucChannelStatus);
    if (CHANNEL_STATUS_ABORT == m_ucChannelStatus)
    {
        LOG4_WARNING("channel_fd[%d], channel_seq[%d], channel_status[%d] send EOF.", m_iFd, m_ulSeq, m_ucChannelStatus);
        return(CODEC_STATUS_EOF);
    }
    E_CODEC_STATUS eCodecStatus = CODEC_STATUS_OK;
    switch (m_ucChannelStatus)
    {
        case CHANNEL_STATUS_ESTABLISHED:
            eCodecStatus = ((CodecHttp*)m_pCodec)->Encode(oHttpMsg, m_pSendBuff);
            m_dKeepAlive = ((CodecHttp*)m_pCodec)->GetKeepAlive();
            break;
        case CHANNEL_STATUS_TELL_WORKER:
        case CHANNEL_STATUS_WORKER:
        case CHANNEL_STATUS_TRANSFER_TO_WORKER:
        case CHANNEL_STATUS_CONNECTED:
        case CHANNEL_STATUS_TRY_CONNECT:
        case CHANNEL_STATUS_INIT:
            eCodecStatus = ((CodecHttp*)m_pCodec)->Encode(oHttpMsg, m_pWaitForSendBuff);
            if (CODEC_STATUS_OK == eCodecStatus)
            {
                eCodecStatus = CODEC_STATUS_PAUSE;
            }
            break;
        default:
            LOG4_ERROR("invalid connect status %d!", m_ucChannelStatus);
            return(CODEC_STATUS_OK);
    }

    if (CODEC_STATUS_OK != eCodecStatus)
    {
        return(eCodecStatus);
    }

    int iNeedWriteLen = m_pSendBuff->ReadableBytes();
    if (iNeedWriteLen <= 0)
    {
        return(eCodecStatus);
    }

    int iWriteLen = Write(m_pSendBuff, m_iErrno);
    if (iWriteLen >= 0)
    {
        m_ulStepSeq = ulStepSeq;
        m_dActiveTime = m_pLabor->GetNowTime();
        if (iNeedWriteLen == iWriteLen)
        {
            if (ulStepSeq == 0 && m_dKeepAlive == 0.0)
            {
                return(CODEC_STATUS_EOF);
            }
            return(CODEC_STATUS_OK);
        }
        else
        {
            return(CODEC_STATUS_PAUSE);
        }
    }
    else
    {
        if (EAGAIN == m_iErrno && EINTR == m_iErrno)    // 对非阻塞socket而言，EAGAIN不是一种错误;EINTR即errno为4，错误描述Interrupted system call，操作也应该继续。
        {
            m_dActiveTime = m_pLabor->GetNowTime();
            return(CODEC_STATUS_PAUSE);
        }
        LOG4_ERROR("send to fd %d error %d: %s",
                m_iFd, m_iErrno, strerror_r(m_iErrno, m_szErrBuff, sizeof(m_szErrBuff)));
        m_strErrMsg = m_szErrBuff;
        return(CODEC_STATUS_INT);
    }
}

E_CODEC_STATUS SocketChannelImpl::Recv(MsgHead& oMsgHead, MsgBody& oMsgBody)
{
    LOG4_TRACE("channel_fd[%d], channel_seq[%d], channel_status[%d]", m_iFd, m_ulSeq, m_ucChannelStatus);
    if (CHANNEL_STATUS_ABORT == m_ucChannelStatus)
    {
        LOG4_WARNING("channel_fd[%d], channel_seq[%d], channel_status[%d] recv EOF.", m_iFd, m_ulSeq, m_ucChannelStatus);
        return(CODEC_STATUS_EOF);
    }
    int iReadLen = 0;
    iReadLen = Read(m_pRecvBuff, m_iErrno);
    LOG4_TRACE("recv from fd %d data len %d. and m_pRecvBuff->ReadableBytes() = %d", m_iFd, iReadLen, m_pRecvBuff->ReadableBytes());
    if (iReadLen > 0)
    {
        m_dActiveTime = m_pLabor->GetNowTime();
        E_CODEC_STATUS eCodecStatus = m_pCodec->Decode(m_pRecvBuff, oMsgHead, oMsgBody);
        if (CODEC_STATUS_OK == eCodecStatus)
        {
            switch (m_ucChannelStatus)
            {
                case CHANNEL_STATUS_ESTABLISHED:
                    if ((gc_uiCmdReq & oMsgHead.cmd()) && (m_strClientData.size() > 0))
                    {
                        m_ulForeignSeq = oMsgHead.seq();
                        ++m_ulUnitTimeMsgNum;
                        ++m_ulMsgNum;
                        oMsgBody.set_add_on(m_strClientData);
                    }
                    break;
                case CHANNEL_STATUS_TELL_WORKER:
                case CHANNEL_STATUS_WORKER:
                case CHANNEL_STATUS_TRANSFER_TO_WORKER:
                case CHANNEL_STATUS_CONNECTED:
                case CHANNEL_STATUS_TRY_CONNECT:
                case CHANNEL_STATUS_INIT:
                {
                    switch (oMsgHead.cmd())
                    {
                        case CMD_RSP_TELL_WORKER:
                            m_ucChannelStatus = CHANNEL_STATUS_ESTABLISHED;
                            break;
                        case CMD_REQ_TELL_WORKER:
                            m_ucChannelStatus = CHANNEL_STATUS_TELL_WORKER;
                            break;
                        case CMD_RSP_CONNECT_TO_WORKER:
                            m_ucChannelStatus = CHANNEL_STATUS_WORKER;
                            break;
                        case CMD_REQ_CONNECT_TO_WORKER:
                            m_ucChannelStatus = CHANNEL_STATUS_TRANSFER_TO_WORKER;
                            break;
                        default:
                            break;
                    }
                }
                    break;
                default:
                    LOG4_ERROR("invalid connect status %d!", m_ucChannelStatus);
                    return(CODEC_STATUS_ERR);
            }
        }
        LOG4_TRACE("channel_fd[%d], channel_seq[%u], cmd[%u], seq[%u]", m_iFd, m_ulSeq, oMsgHead.cmd(), oMsgHead.seq());
        return(eCodecStatus);
    }
    else if (iReadLen == 0)
    {
        LOG4_DEBUG("fd %d closed by peer, error %d %s!",
                        m_iFd, m_iErrno, strerror_r(m_iErrno, m_szErrBuff, sizeof(m_szErrBuff)));
        m_strErrMsg = m_szErrBuff;
        return(CODEC_STATUS_EOF);
    }
    else
    {
        if (EAGAIN == m_iErrno && EINTR == m_iErrno)    // 对非阻塞socket而言，EAGAIN不是一种错误;EINTR即errno为4，错误描述Interrupted system call，操作也应该继续。
        {
            m_dActiveTime = m_pLabor->GetNowTime();
            return(CODEC_STATUS_PAUSE);
        }
        LOG4_ERROR("recv from fd %d error %d: %s",
                m_iFd, m_iErrno, strerror_r(m_iErrno, m_szErrBuff, sizeof(m_szErrBuff)));
        m_strErrMsg = m_szErrBuff;
        return(CODEC_STATUS_INT);
    }
}

E_CODEC_STATUS SocketChannelImpl::Recv(HttpMsg& oHttpMsg)
{
    LOG4_TRACE("channel_fd[%d], channel_seq[%d]", m_iFd, m_ulSeq);
    if (CHANNEL_STATUS_ABORT == m_ucChannelStatus)
    {
        LOG4_WARNING("channel_fd[%d], channel_seq[%d], channel_status[%d] recv EOF.", m_iFd, m_ulSeq, m_ucChannelStatus);
        return(CODEC_STATUS_EOF);
    }
    int iReadLen = 0;
    iReadLen = Read(m_pRecvBuff, m_iErrno);
    LOG4_TRACE("recv from fd %d data len %d. and m_pRecvBuff->ReadableBytes() = %d",
            m_iFd, iReadLen, m_pRecvBuff->ReadableBytes());
    if (iReadLen > 0)
    {
        m_dActiveTime = m_pLabor->GetNowTime();
        E_CODEC_STATUS eCodecStatus = ((CodecHttp*)m_pCodec)->Decode(m_pRecvBuff, oHttpMsg);
        m_dKeepAlive = (((CodecHttp*)m_pCodec)->GetKeepAlive() >= 0.0) ? ((CodecHttp*)m_pCodec)->GetKeepAlive() : m_dKeepAlive;
        return(eCodecStatus);
    }
    else if (iReadLen == 0)
    {
        LOG4_DEBUG("fd %d closed by peer, error %d %s!",
                        m_iFd, m_iErrno, strerror_r(m_iErrno, m_szErrBuff, sizeof(m_szErrBuff)));
        if (m_pRecvBuff->ReadableBytes() > 0)
        {
            E_CODEC_STATUS eCodecStatus = ((CodecHttp*)m_pCodec)->Decode(m_pRecvBuff, oHttpMsg);
            if (CODEC_STATUS_PAUSE == eCodecStatus || CODEC_STATUS_OK == eCodecStatus)
            {
                oHttpMsg.set_is_decoding(false);
            }
        }
        m_strErrMsg = m_szErrBuff;
        return(CODEC_STATUS_EOF);
    }
    else
    {
        if (EAGAIN == m_iErrno && EINTR == m_iErrno)    // 对非阻塞socket而言，EAGAIN不是一种错误;EINTR即errno为4，错误描述Interrupted system call，操作也应该继续。
        {
            m_dActiveTime = m_pLabor->GetNowTime();
            return(CODEC_STATUS_PAUSE);
        }
        LOG4_ERROR("recv from fd %d error %d: %s",
                m_iFd, m_iErrno, strerror_r(m_iErrno, m_szErrBuff, sizeof(m_szErrBuff)));
        m_strErrMsg = m_szErrBuff;
        return(CODEC_STATUS_INT);
    }
}

E_CODEC_STATUS SocketChannelImpl::Recv(MsgHead& oMsgHead, MsgBody& oMsgBody, HttpMsg& oHttpMsg)
{
    LOG4_TRACE("channel_fd[%d], channel_seq[%d]", m_iFd, m_ulSeq);
    if (CHANNEL_STATUS_ABORT == m_ucChannelStatus)
    {
        LOG4_WARNING("channel_fd[%d], channel_seq[%d], channel_status[%d] recv EOF.", m_iFd, m_ulSeq, m_ucChannelStatus);
        return(CODEC_STATUS_EOF);
    }
    int iReadLen = 0;
    iReadLen = Read(m_pRecvBuff, m_iErrno);
    LOG4_TRACE("recv from fd %d data len %d. and m_pRecvBuff->ReadableBytes() = %d",
            m_iFd, iReadLen, m_pRecvBuff->ReadableBytes());
    if (iReadLen > 0)
    {
        m_dActiveTime = m_pLabor->GetNowTime();
        if (CODEC_HTTP == m_pCodec->GetCodecType())
        {
            E_CODEC_STATUS eCodecStatus = ((CodecHttp*)m_pCodec)->Decode(m_pRecvBuff, oHttpMsg);
            m_dKeepAlive = (((CodecHttp*)m_pCodec)->GetKeepAlive() >= 0.0) ? ((CodecHttp*)m_pCodec)->GetKeepAlive() : m_dKeepAlive;
            return(eCodecStatus);
        }
        else
        {
            E_CODEC_STATUS eCodecStatus = m_pCodec->Decode(m_pRecvBuff, oMsgHead, oMsgBody);
            if ((CODEC_STATUS_OK == eCodecStatus) && (gc_uiCmdReq & oMsgHead.cmd()) && (m_strClientData.size() > 0))
            {
                m_ulForeignSeq = oMsgHead.seq();
                ++m_ulUnitTimeMsgNum;
                ++m_ulMsgNum;
                oMsgBody.set_add_on(m_strClientData);
                LOG4_TRACE("channel_fd[%d], channel_seq[%u], cmd[%u], seq[%u]", m_iFd, m_ulSeq, oMsgHead.cmd(), oMsgHead.seq());
            }
            return(eCodecStatus);
        }
    }
    else if (iReadLen == 0)
    {
        LOG4_DEBUG("fd %d closed by peer, error %d %s!",
                        m_iFd, m_iErrno, strerror_r(m_iErrno, m_szErrBuff, sizeof(m_szErrBuff)));
        m_strErrMsg = m_szErrBuff;
        return(CODEC_STATUS_EOF);
    }
    else
    {
        if (EAGAIN == m_iErrno && EINTR == m_iErrno)    // 对非阻塞socket而言，EAGAIN不是一种错误;EINTR即errno为4，错误描述Interrupted system call，操作也应该继续。
        {
            m_dActiveTime = m_pLabor->GetNowTime();
            return(CODEC_STATUS_PAUSE);
        }
        LOG4_ERROR("recv from fd %d error %d: %s",
                m_iFd, m_iErrno, strerror_r(m_iErrno, m_szErrBuff, sizeof(m_szErrBuff)));
        m_strErrMsg = m_szErrBuff;
        return(CODEC_STATUS_INT);
    }
}

E_CODEC_STATUS SocketChannelImpl::Fetch(MsgHead& oMsgHead, MsgBody& oMsgBody)
{
    LOG4_TRACE("channel_fd[%d], channel_seq[%d]", m_iFd, m_ulSeq);
    if (CHANNEL_STATUS_ABORT == m_ucChannelStatus)
    {
        LOG4_WARNING("channel_fd[%d], channel_seq[%d], channel_status[%d] recv EOF.", m_iFd, m_ulSeq, m_ucChannelStatus);
        return(CODEC_STATUS_EOF);
    }
    LOG4_TRACE("fetch from fd %d and m_pRecvBuff->ReadableBytes() = %d",
            m_iFd, m_pRecvBuff->ReadableBytes());
    E_CODEC_STATUS eCodecStatus = m_pCodec->Decode(m_pRecvBuff, oMsgHead, oMsgBody);
    if (CODEC_STATUS_OK == eCodecStatus)
    {
        m_ulForeignSeq = oMsgHead.seq();
        ++m_ulUnitTimeMsgNum;
        ++m_ulMsgNum;
        LOG4_TRACE("channel_fd[%d], channel_seq[%u], cmd[%u], seq[%u]", m_iFd, m_ulSeq, oMsgHead.cmd(), oMsgHead.seq());
    }
    return(eCodecStatus);
}

E_CODEC_STATUS SocketChannelImpl::Fetch(HttpMsg& oHttpMsg)
{
    // TODO 当http1.0响应包未带Content-Length头时，以关闭连接表示数据发送完毕。需再处理
    LOG4_TRACE("channel_fd[%d], channel_seq[%d]", m_iFd, m_ulSeq);
    if (CHANNEL_STATUS_ABORT == m_ucChannelStatus)
    {
        LOG4_WARNING("channel_fd[%d], channel_seq[%d], channel_status[%d] recv EOF.", m_iFd, m_ulSeq, m_ucChannelStatus);
        return(CODEC_STATUS_EOF);
    }
    E_CODEC_STATUS eCodecStatus = ((CodecHttp*)m_pCodec)->Decode(m_pRecvBuff, oHttpMsg);
    m_dKeepAlive = (((CodecHttp*)m_pCodec)->GetKeepAlive() >= 0.0) ? ((CodecHttp*)m_pCodec)->GetKeepAlive() : m_dKeepAlive;
    return(eCodecStatus);
}

E_CODEC_STATUS SocketChannelImpl::Fetch(MsgHead& oMsgHead, MsgBody& oMsgBody, HttpMsg& oHttpMsg)
{
    LOG4_TRACE("channel_fd[%d], channel_seq[%d]", m_iFd, m_ulSeq);
    if (CHANNEL_STATUS_ABORT == m_ucChannelStatus)
    {
        return(CODEC_STATUS_EOF);
    }
    E_CODEC_STATUS eCodecStatus = CODEC_STATUS_OK;
    if (CODEC_HTTP == m_pCodec->GetCodecType())
    {
        eCodecStatus = ((CodecHttp*)m_pCodec)->Decode(m_pRecvBuff, oHttpMsg);
        m_dKeepAlive = (((CodecHttp*)m_pCodec)->GetKeepAlive() >= 0.0) ? ((CodecHttp*)m_pCodec)->GetKeepAlive() : m_dKeepAlive;
        return(eCodecStatus);
    }
    else
    {
        eCodecStatus = m_pCodec->Decode(m_pRecvBuff, oMsgHead, oMsgBody);
        if (CODEC_STATUS_OK == eCodecStatus)
        {
            m_ulForeignSeq = oMsgHead.seq();
            ++m_ulUnitTimeMsgNum;
            ++m_ulMsgNum;
            LOG4_TRACE("channel_fd[%d], channel_seq[%u], cmd[%u], seq[%u]", m_iFd, m_ulSeq, oMsgHead.cmd(), oMsgHead.seq());
        }
        return(eCodecStatus);
    }
}

bool SocketChannelImpl::SwitchCodec(E_CODEC_TYPE eCodecType, ev_tstamp dKeepAlive)
{
    LOG4_TRACE("channel_fd[%d], channel_seq[%d], codec_type[%d], new_codec_type[%d]",
                    m_iFd, m_ulSeq, m_pCodec->GetCodecType(), eCodecType);
    if (eCodecType == m_pCodec->GetCodecType())
    {
        return(true);
    }

    Codec* pNewCodec = NULL;
    try
    {
        switch (eCodecType)
        {
            case CODEC_NEBULA:
                pNewCodec = new CodecProto(m_pLogger, eCodecType, m_strKey);
                break;
            case CODEC_PRIVATE:
                pNewCodec = new CodecPrivate(m_pLogger, eCodecType, m_strKey);
                break;
            case CODEC_HTTP:
                pNewCodec = new CodecHttp(m_pLogger, eCodecType, m_strKey);
                break;
            default:
                LOG4_ERROR("no codec defined for code type %d", eCodecType);
                break;
        }
    }
    catch(std::bad_alloc& e)
    {
        LOG4_ERROR("%s", e.what());
        return(false);
    }
    DELETE(m_pCodec);
    m_pCodec = pNewCodec;
    m_dKeepAlive = dKeepAlive;
    m_dActiveTime = m_pLabor->GetNowTime();
    return(true);
}

ev_io* SocketChannelImpl::AddIoWatcher()
{
    m_pIoWatcher = (ev_io*)malloc(sizeof(ev_io));
    if (NULL != m_pIoWatcher)
    {
        m_pIoWatcher->data = m_pSocketChannel;      // (void*)(Channel*)
        m_pIoWatcher->fd = GetFd();
    }
    return(m_pIoWatcher);
}

ev_timer* SocketChannelImpl::AddTimerWatcher()
{
    m_pTimerWatcher = (ev_timer*)malloc(sizeof(ev_timer));
    if (NULL != m_pTimerWatcher)
    {
        m_pTimerWatcher->data = m_pSocketChannel;    // (void*)(Channel*)
    }
    return(m_pTimerWatcher);
}

void SocketChannelImpl::Abort()
{
    if (CHANNEL_STATUS_ABORT != m_ucChannelStatus)
    {
        m_ucChannelStatus = CHANNEL_STATUS_ABORT;
        m_pSendBuff->Compact(1);
        m_pWaitForSendBuff->Compact(1);
        close(m_iFd);
    }
}

int SocketChannelImpl::Write(CBuffer* pBuff, int& iErrno)
{
    return(pBuff->WriteFD(m_iFd, iErrno));
}

int SocketChannelImpl::Read(CBuffer* pBuff, int& iErrno)
{
    return(pBuff->ReadFD(m_iFd, iErrno));
}


} /* namespace neb */
