/*
* Copyright (c) 2019 ChangSha JuSong Soft Inc. <service@pinusdb.cn>.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; version 3 of the License.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.

* You should have received a copy of the GNU General Public License
* along with this program; If not, see <http://www.gnu.org/licenses>
*/

#include "storage/normal_part_idx.h"
#include "util/string_tool.h"
#include "util/log_util.h"
#include "util/date_time.h"

#define NORMALIDX_FILE_TYPE_STR_LEN  16
#define NORMALIDX_FILE_TYPE_STR      "NORMAL IDX 1"

#define NORMALIDX_FILE_BLOCK_SIZE   (1024 * 1024)

typedef struct _NormalIdxMeta
{
  char fileType_[NORMALIDX_FILE_TYPE_STR_LEN];
  uint32_t partCode_;
  char padding_[8];
  uint32_t crc_;
}NormalIdxMeta;

typedef struct _NormalIdxItem
{
  int64_t devId_;
  int64_t idxTs_;
  int32_t pageNo_;
  char padding_[8];
  uint32_t crc_;
}NormalIdxItem;

DataIdxComp dataIdxComp_;

NormalPartIdx::NormalPartIdx()
{
  curPos_ = 0;
  bgDayTs_ = 0;
  edDayTs_ = 0;
  readOnly_ = true;
  maxPageNo_ = 0;
}

NormalPartIdx::~NormalPartIdx()
{
  for (auto devIt = idxMap_.begin(); devIt != idxMap_.end(); devIt++)
  {
    delete devIt->second;
  }
  idxMap_.clear();
  idxFile_.Close();
}

PdbErr_t NormalPartIdx::Create(const char* pPath, int32_t partCode)
{
  PdbErr_t retVal = PdbE_OK;
  OSFile osFile;
  NormalIdxMeta idxMeta;

  if (partCode < 0 || partCode > 365 * 10000)
    return PdbE_INVALID_PARAM;

  retVal = osFile.OpenNew(pPath);
  if (retVal != PdbE_OK)
    return retVal;

  do {
    retVal = osFile.GrowTo(NORMALIDX_FILE_BLOCK_SIZE);
    if (retVal != PdbE_OK)
      break;

    memset(&idxMeta, 0, sizeof(NormalIdxMeta));
    strncpy(idxMeta.fileType_, NORMALIDX_FILE_TYPE_STR, sizeof(idxMeta.fileType_));
    idxMeta.partCode_ = partCode;
    idxMeta.crc_ = StringTool::CRC32(&idxMeta, (sizeof(NormalIdxMeta) - 4));

    retVal = osFile.Write(&idxMeta, sizeof(NormalIdxMeta), 0);
  } while (false);

  osFile.Close();
  if (retVal != PdbE_OK)
  {
    FileTool::RemoveFile(pPath);
  }

  return retVal;
}

PdbErr_t NormalPartIdx::Open(const char* pPath, bool readOnly)
{
  PdbErr_t retVal = PdbE_OK;
  curPos_ = 0;
  bgDayTs_ = 0;
  edDayTs_ = 0;
  maxPageNo_ = 0;
  readOnly_ = readOnly;
  idxPath_ = pPath;

  std::unique_lock<std::mutex> idxLock(idxMutex_);
  std::unique_lock<std::mutex> fileLock(fileMutex_);

  retVal = idxFile_.OpenNormal(pPath, readOnly);
  if (retVal != PdbE_OK)
    return retVal;

  curPos_ = sizeof(NormalIdxItem);
  Arena tmpArena;
  char* pTmpBuf = tmpArena.Allocate(NORMALIDX_FILE_BLOCK_SIZE);
  if (pTmpBuf == nullptr)
    return PdbE_NOMEM;

  size_t fileSize = idxFile_.FileSize();
  if (fileSize % NORMALIDX_FILE_BLOCK_SIZE != 0)
    return PdbE_IDX_FILE_ERROR;

  retVal = idxFile_.Read(pTmpBuf, NORMALIDX_FILE_BLOCK_SIZE, 0);
  if (retVal != PdbE_OK)
    return retVal;

  //1.1 验证索引文件
  const NormalIdxMeta* pMeta = (const NormalIdxMeta*)pTmpBuf;
  if (pMeta->crc_ != StringTool::CRC32(pTmpBuf, (sizeof(NormalIdxMeta) - 4)))
  {
    LOG_ERROR("failed to open normal index file ({}), file meta crc error ", pPath);
    return PdbE_IDX_FILE_ERROR;
  }

  //1.2 验证文件版本
  if (strncmp(pMeta->fileType_, NORMALIDX_FILE_TYPE_STR, NORMALIDX_FILE_TYPE_STR_LEN) != 0)
  {
    LOG_ERROR("failed to open normal index file ({}), field list mismatch", pPath);
    return PdbE_IDX_FILE_ERROR;
  }

  //1.2 验证开始文件所属天
  if (pMeta->partCode_ < 0 || pMeta->partCode_ > 365 * 10000)
  {
    LOG_ERROR("failed to open normal index file ({}), datapart code ({}) error",
      pPath, pMeta->partCode_);
    return PdbE_IDX_FILE_ERROR;
  }

  bgDayTs_ = MillisPerDay * pMeta->partCode_;
  edDayTs_ = bgDayTs_ + MillisPerDay;

  //2. 读取数据内容
  size_t readPos = 0;
  const char* pTmpItem = pTmpBuf;
  while (true)
  {
    pTmpItem += sizeof(NormalIdxItem);
    if (pTmpItem >= (pTmpBuf + NORMALIDX_FILE_BLOCK_SIZE))
    {
      readPos += NORMALIDX_FILE_BLOCK_SIZE;
      if (readPos >= fileSize)
        break;

      retVal = idxFile_.Read(pTmpBuf, NORMALIDX_FILE_BLOCK_SIZE, readPos);
      if (retVal != PdbE_OK)
        return retVal;

      pTmpItem = pTmpBuf;
    }

    const NormalIdxItem* pIdxItem = (const NormalIdxItem*)pTmpItem;
    if (pIdxItem->crc_ != 0 && pIdxItem->devId_ > 0)
    {
      DataIdxSkipList* pIdxSk = nullptr;
      auto idxIt = idxMap_.find(pIdxItem->devId_);
      if (idxIt == idxMap_.end())
      {
        pIdxSk = new DataIdxSkipList(dataIdxComp_, &arena_);
        idxMap_.insert(std::pair<int64_t, DataIdxSkipList*>(pIdxItem->devId_, pIdxSk));
      }
      else
      {
        pIdxSk = idxIt->second;
      }

      if (pIdxItem->idxTs_ < bgDayTs_ || pIdxItem->idxTs_ >= edDayTs_)
      {
        LOG_ERROR("normal index file ({}), position ({}) error", pPath, curPos_);
        return PdbE_IDX_FILE_ERROR;
      }

      pIdxSk->Insert(static_cast<uint32_t>(pIdxItem->idxTs_ - bgDayTs_), pIdxItem->pageNo_);
      curPos_ += sizeof(NormalIdxItem);

      if (pIdxItem->pageNo_ > maxPageNo_)
        maxPageNo_ = pIdxItem->pageNo_;
    }
    else
    {
      break;
    }
  }

  return PdbE_OK;
}

PdbErr_t NormalPartIdx::Close()
{
  std::unique_lock<std::mutex> idxLock(idxMutex_);
  std::unique_lock<std::mutex> fileLock(fileMutex_);
  idxFile_.Close();
  curPos_ = 0;
  for (auto devIt = idxMap_.begin(); devIt != idxMap_.end(); devIt++)
  {
    delete devIt->second;
  }
  idxMap_.clear();

  return PdbE_OK;
}

PdbErr_t NormalPartIdx::AddIdx(int64_t devId, int64_t idxTs, int32_t pageNo)
{
  if (devId <= 0 || idxTs < bgDayTs_ || idxTs >= edDayTs_ || pageNo <= 0)
    return PdbE_INVALID_PARAM;

  uint32_t partTs = static_cast<uint32_t>(idxTs - bgDayTs_);

  std::unique_lock<std::mutex> idxLock(idxMutex_);
  DataIdxSkipList* pIdxSk = nullptr;
  auto idxIt = idxMap_.find(devId);
  if (idxIt == idxMap_.end())
  {
    pIdxSk = new DataIdxSkipList(dataIdxComp_, &arena_);
    idxMap_.insert(std::pair<int64_t, DataIdxSkipList*>(devId, pIdxSk));
  }
  else
  {
    pIdxSk = idxIt->second;
  }

  pIdxSk->Insert(partTs, pageNo);
  return PdbE_OK;
}

PdbErr_t NormalPartIdx::WriteIdx(const std::vector<NormalPageIdx>& idxVec)
{
  if (idxVec.size() == 0)
    return PdbE_OK;

  PdbErr_t retVal = PdbE_OK;
  std::unique_lock<std::mutex> fileLock(fileMutex_);
  Arena tmpArena;
  size_t idxBufLen = idxVec.size() * sizeof(NormalIdxItem);
  char* pTmpBuf = tmpArena.Allocate(idxBufLen);
  if (pTmpBuf == nullptr)
    return PdbE_NOMEM;

  memset(pTmpBuf, 0, idxBufLen);
  unsigned char* pTmpItem = (unsigned char*)pTmpBuf;
  for (auto idxIt = idxVec.begin(); idxIt != idxVec.end(); idxIt++)
  {
    NormalIdxItem* pItem = (NormalIdxItem*)pTmpItem;
    pItem->devId_ = idxIt->devId_;
    pItem->pageNo_ = idxIt->pageNo_;
    pItem->idxTs_ = idxIt->idxTs_;
    pItem->crc_ = StringTool::CRC32(pTmpItem, (sizeof(NormalIdxItem) - 4));
    pTmpItem += sizeof(NormalIdxItem);
  }

  size_t idxFileSize = idxFile_.FileSize();
  if ((curPos_ + idxBufLen) >= idxFileSize)
  {
    retVal = idxFile_.Grow(NORMALIDX_FILE_BLOCK_SIZE);
    if (retVal != PdbE_OK)
      return retVal;
  }
  retVal = idxFile_.Write(pTmpBuf, idxBufLen, curPos_);
  if (retVal != PdbE_OK)
    return retVal;

  idxFile_.Sync();
  curPos_ += idxBufLen;

  return retVal;
}

PdbErr_t NormalPartIdx::GetIndex(int64_t devId, int64_t ts, NormalPageIdx* pIdx)
{
  PdbErr_t retVal = PdbE_OK;
  if (pIdx == nullptr)
    return PdbE_INVALID_PARAM;

  uint32_t partTs = 0; 
  if (ts >= edDayTs_)
    partTs = static_cast<uint32_t>(MillisPerDay - 1);
  else if (ts >= bgDayTs_)
    partTs = static_cast<uint32_t>(ts - bgDayTs_);

  std::unique_lock<std::mutex> idxLock(idxMutex_);
  auto devIt = idxMap_.find(devId);
  if (devIt != idxMap_.end())
  {
    DataIdxSkipList::Iterator idxIter(devIt->second);
    idxIter.Seek(partTs);
    if (idxIter.Valid())
    {
      pIdx->devId_ = devId;
      pIdx->pageNo_ = idxIter.GetVal();
      pIdx->idxTs_ = bgDayTs_ + idxIter.GetKey();
      return PdbE_OK;
    }

    return PdbE_IDX_NOT_FOUND;
  }

  return PdbE_DEV_NOT_FOUND;
}

PdbErr_t NormalPartIdx::GetPrevIndex(int64_t devId, int64_t ts, NormalPageIdx* pIdx)
{
  PdbErr_t retVal = PdbE_OK;
  if (pIdx == nullptr)
    return PdbE_INVALID_PARAM;

  uint32_t partTs = 0;
  if (ts >= edDayTs_)
    partTs = static_cast<uint32_t>(MillisPerDay - 1);
  else if (ts >= bgDayTs_)
    partTs = static_cast<uint32_t>(ts - bgDayTs_);

  std::unique_lock<std::mutex> idxLock(idxMutex_);
  auto devIt = idxMap_.find(devId);
  if (devIt != idxMap_.end())
  {
    DataIdxSkipList::Iterator idxIter(devIt->second);
    idxIter.Seek(partTs);
    if (!idxIter.Valid())
      return PdbE_IDX_NOT_FOUND;

    idxIter.Next();
    if (idxIter.Valid())
    {
      pIdx->devId_ = devId;
      pIdx->pageNo_ = idxIter.GetVal();
      pIdx->idxTs_ = bgDayTs_ + idxIter.GetKey();
      return PdbE_OK;
    }

    return PdbE_IDX_NOT_FOUND;
  }

  return PdbE_DEV_NOT_FOUND;
}

PdbErr_t NormalPartIdx::GetNextIndex(int64_t devId, int64_t ts, NormalPageIdx* pIdx)
{
  PdbErr_t retVal = PdbE_OK;
  if (pIdx == nullptr)
    return PdbE_INVALID_PARAM;

  uint32_t partTs = static_cast<uint32_t>(ts - bgDayTs_);

  std::unique_lock<std::mutex> idxLock(idxMutex_);
  auto devIt = idxMap_.find(devId);
  if (devIt != idxMap_.end())
  {
    DataIdxSkipList::Iterator idxIter(devIt->second);
    idxIter.Seek(partTs);
    if (!idxIter.Valid())
      return PdbE_IDX_NOT_FOUND;

    idxIter.Prev();
    if (idxIter.Valid())
    {
      pIdx->devId_ = devId;
      pIdx->pageNo_ = idxIter.GetVal();
      pIdx->idxTs_ = bgDayTs_ + idxIter.GetKey();
      return PdbE_OK;
    }

    return PdbE_IDX_NOT_FOUND;
  }

  return PdbE_DEV_NOT_FOUND;
}

void NormalPartIdx::GetAllDevId(std::vector<int64_t>& devIdVec)
{
  devIdVec.resize(idxMap_.size());
  size_t pos = 0;
  for (auto idxIt = idxMap_.begin(); idxIt != idxMap_.end(); idxIt++)
  {
    devIdVec[pos++] = idxIt->first;
  }

  std::sort(devIdVec.begin(), devIdVec.end());
}
