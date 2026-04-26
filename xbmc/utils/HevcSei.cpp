/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "HevcSei.h"
#include "HDR10Plus.h"

#include "utils/log.h"

void CBitstreamWriter::WriteBits(uint32_t value, int numBits)
{
  while (numBits > 0)
  {
    int bitPos = m_posBits % 8;
    int bitsToWrite = std::min(8 - bitPos, numBits);
    uint8_t mask = (1 << bitsToWrite) - 1;
    uint8_t byteValue = (value & mask) << (8 - bitPos - bitsToWrite);

    if (bitPos == 0)
    {
      m_buffer.push_back(byteValue);
    }
    else
    {
      m_buffer.back() |= byteValue;
    }

    value >>= bitsToWrite;
    m_posBits += bitsToWrite;
    numBits -= bitsToWrite;
  }
}

void CBitstreamWriter::WriteByte(uint8_t byte)
{
  ByteAlign();
  m_buffer.push_back(byte);
  m_posBits += 8;
}

void CBitstreamWriter::ByteAlign()
{
  if (m_posBits % 8 != 0)
  {
    m_buffer.push_back(0);
    m_posBits = (m_posBits / 8 + 1) * 8;
  }
}

std::vector<uint8_t> CBitstreamWriter::GetData() const
{
  return m_buffer;
}

std::vector<uint8_t> BuildMasteringDisplaySei(const MasteringDisplayColourVolume& mdcv)
{
  CBitstreamWriter w;
  for (int i = 0; i < 3; i++) {
    w.WriteBits(mdcv.displayPrimaries[i].x, 16);
    w.WriteBits(mdcv.displayPrimaries[i].y, 16);
  }
  w.WriteBits(mdcv.whitePoint.x, 16);
  w.WriteBits(mdcv.whitePoint.y, 16);
  w.WriteBits(mdcv.maxLuminance, 32);
  w.WriteBits(mdcv.minLuminance, 32);
  w.ByteAlign(); // Ensure byte alignment for SEI payload
  return w.GetData();
}

std::vector<uint8_t> BuildContentLightLevelSei(const ContentLightLevel& cll)
{
  CBitstreamWriter w;
  w.ByteAlign(); // Ensure byte alignment before writing
  w.WriteBits(cll.maxContentLightLevel, 16);
  w.WriteBits(cll.maxFrameAverageLightLevel, 16);
  return w.GetData();
}

MasteringDisplayColourVolume GetDefaultMasteringDisplay()
{
  // Default P3-D65 mastering display
  MasteringDisplayColourVolume mdcv;
  mdcv.displayPrimaries[0] = { 13250, 34500 }; // R
  mdcv.displayPrimaries[1] = { 7500, 30000 };  // G
  mdcv.displayPrimaries[2] = { 3000, 15000 };  // B
  mdcv.whitePoint = { 15635, 16450 };          // D65
  mdcv.maxLuminance = 1000000;                 // 1000 nits
  mdcv.minLuminance = 1;                       // 0.001 nits
  return mdcv;
}

ContentLightLevel GetDefaultContentLightLevel()
{
  ContentLightLevel cll;
  cll.maxContentLightLevel = 1000;            // 1000 nits MaxCLL (reasonable default)
  cll.maxFrameAverageLightLevel = 200;        // 200 nits MaxFALL (reasonable default)
  return cll;
}

void HevcAddStartCodeEmulationPrevention3Byte(std::vector<uint8_t>& buf)
{
  size_t i = 0;

  while (i < buf.size())
  {
    if (i > 2 && buf[i - 2] == 0 && buf[i - 1] == 0 && buf[i] <= 3)
      buf.insert(buf.begin() + i, 3);

    i += 1;
  }
}

void HevcClearStartCodeEmulationPrevention3Byte(const uint8_t* buf,
                                                const size_t len,
                                                std::vector<uint8_t>& out)
{
  size_t i = 0;

  if (len > 2)
  {
    out.reserve(len);

    out.emplace_back(buf[0]);
    out.emplace_back(buf[1]);

    for (i = 2; i < len; i++)
    {
      if (!(buf[i - 2] == 0 && buf[i - 1] == 0 && buf[i] == 3))
        out.emplace_back(buf[i]);
    }
  }
  else
  {
    out.assign(buf, buf + len);
  }
}

int CHevcSei::ParseSeiMessage(CBitstreamReader& br, std::vector<CHevcSei>& messages)
{
  CHevcSei sei;
  uint8_t lastPayloadTypeByte{0};
  uint8_t lastPayloadSizeByte{0};

  sei.m_msgOffset = br.Position() / 8;

  lastPayloadTypeByte = br.ReadBits(8);
  while (lastPayloadTypeByte == 0xFF)
  {
    lastPayloadTypeByte = br.ReadBits(8);
    sei.m_payloadType += 255;
  }

  sei.m_payloadType += lastPayloadTypeByte;

  lastPayloadSizeByte = br.ReadBits(8);
  while (lastPayloadSizeByte == 0xFF)
  {
    lastPayloadSizeByte = br.ReadBits(8);
    sei.m_payloadSize += 255;
  }

  sei.m_payloadSize += lastPayloadSizeByte;
  sei.m_payloadOffset = br.Position() / 8;

  // Invalid size
  if (sei.m_payloadSize > br.AvailableBits())
    return 1;

  br.SkipBits(sei.m_payloadSize * 8);
  messages.emplace_back(sei);

  return 0;
}

std::vector<CHevcSei> CHevcSei::ParseSeiRbspInternal(const uint8_t* buf, const size_t len)
{
  std::vector<CHevcSei> messages;

  if (len > 4)
  {
    CBitstreamReader br(buf, len);

    // forbidden_zero_bit, nal_type, nuh_layer_id, temporal_id
    // nal_type == SEI_PREFIX should already be verified by caller
    br.SkipBits(16);

    while (true)
    {
      if (ParseSeiMessage(br, messages))
        break;

      if (br.AvailableBits() <= 8)
        break;
    }
  }

  return messages;
}

std::vector<CHevcSei> CHevcSei::ParseSeiRbsp(const uint8_t* buf, const size_t len)
{
  return ParseSeiRbspInternal(buf, len);
}

std::vector<CHevcSei> CHevcSei::ParseSeiRbspUnclearedEmulation(const uint8_t* inData,
                                                               const size_t inDataLen,
                                                               std::vector<uint8_t>& buf)
{
  HevcClearStartCodeEmulationPrevention3Byte(inData, inDataLen, buf);
  return ParseSeiRbsp(buf.data(), buf.size());
}

std::optional<const CHevcSei*> CHevcSei::FindHdr10PlusSeiMessage(
    const std::vector<uint8_t>& buf, const std::vector<CHevcSei>& messages)
{
  for (const CHevcSei& sei : messages)
  {
    // User Data Registered ITU-T T.35
    if (sei.m_payloadType == 4 && sei.m_payloadSize >= 7)
    {
      CBitstreamReader br(buf.data() + sei.m_payloadOffset, sei.m_payloadSize);
      const auto itu_t_t35_country_code = br.ReadBits(8);
      const auto itu_t_t35_terminal_provider_code = br.ReadBits(16);
      const auto itu_t_t35_terminal_provider_oriented_code = br.ReadBits(16);

      // United States, Samsung Electronics America, ST 2094-40
      if (itu_t_t35_country_code == 0xB5 && itu_t_t35_terminal_provider_code == 0x003C &&
          itu_t_t35_terminal_provider_oriented_code == 0x0001)
      {
        const auto application_identifier = br.ReadBits(8);
        const auto application_version = br.ReadBits(8);

        if (application_identifier == 4 && application_version <= 1)
          return &sei;
      }
    }
  }

  return {};
}

const std::optional<const Hdr10PlusMetadata> CHevcSei::ExtractHdr10Plus(
  const std::vector<CHevcSei>& messages,
  const std::vector<uint8_t>& buf)
{
  for (const CHevcSei& sei : messages)
  {
    // User Data Registered ITU-T T.35
    if (sei.m_payloadType == 4 && sei.m_payloadSize >= 7)
    {
      const unsigned char* data = buf.data() + sei.m_payloadOffset;
      size_t size = sei.m_payloadSize;

      CBitstreamReader br(data, size);
      const auto itu_t_t35_country_code = br.ReadBits(8);
      const auto itu_t_t35_terminal_provider_code = br.ReadBits(16);
      const auto itu_t_t35_terminal_provider_oriented_code = br.ReadBits(16);

      // United States, Samsung Electronics America, ST 2094-40
      if (itu_t_t35_country_code == 0xB5 && itu_t_t35_terminal_provider_code == 0x003C &&
          itu_t_t35_terminal_provider_oriented_code == 0x0001)
      {
        const auto application_identifier = br.ReadBits(8);
        const auto application_version = br.ReadBits(8);

        if (application_identifier == 4 && application_version <= 1) {
          CBitstreamReader br2(data, size);
          return hdr10plus_sei_to_metadata(br2);
        }
      }
    }
  }

  return std::nullopt;
}

const std::optional<MasteringDisplayColourVolume> CHevcSei::ExtractMasteringDisplayColourVolume(
  const std::vector<CHevcSei>& messages,
  const std::vector<uint8_t>& buf) {

  for (const auto& sei : messages) {

    // Check for Mastering Display Metadata SEI (payload type 137)
    if (sei.m_payloadType == 137 && sei.m_payloadSize >= 24) {

      CBitstreamReader br(buf.data() + sei.m_payloadOffset, sei.m_payloadSize);

      MasteringDisplayColourVolume metadata;

      // Read display primaries
      for (int i = 0; i < 3; ++i) {
          metadata.displayPrimaries[i].x = br.ReadBits(16);
          metadata.displayPrimaries[i].y = br.ReadBits(16);
      }

      // Read white point
      metadata.whitePoint.x = br.ReadBits(16);
      metadata.whitePoint.y = br.ReadBits(16);

      // Read max and min luminance
      uint32_t maxLuminanceRaw = br.ReadBits(32);
      uint32_t minLuminanceRaw = br.ReadBits(32);

      // Convert to nits (cd/m²) for max only.
      metadata.maxLuminance = static_cast<uint32_t>(maxLuminanceRaw) / 10000.0f;
      metadata.minLuminance = static_cast<uint32_t>(minLuminanceRaw);

      return metadata;
    }
  }
  return std::nullopt;
}

const std::optional<ContentLightLevel> CHevcSei::ExtractContentLightLevel(
  const std::vector<CHevcSei>& messages,
  const std::vector<uint8_t>& buf) {

  for (const auto& sei : messages) {

    // Check for Content Light Level Information SEI (payload type 144)
    if (sei.m_payloadType == 144 && sei.m_payloadSize >= 4) {

        CBitstreamReader br(buf.data() + sei.m_payloadOffset, sei.m_payloadSize);

        uint16_t maxCLL = br.ReadBits(16);
        uint16_t maxFALL = br.ReadBits(16);

        return ContentLightLevel{maxCLL, maxFALL};
    }
  }
  return std::nullopt;
}

const std::vector<uint8_t> CHevcSei::RemoveHdr10PlusFromSeiNalu(const uint8_t* inData, const size_t inDataLen)
{

  std::vector<uint8_t> buf;
  std::vector<CHevcSei> messages = CHevcSei::ParseSeiRbspUnclearedEmulation(inData, inDataLen, buf);

  if (auto res = CHevcSei::FindHdr10PlusSeiMessage(buf, messages))
  {
    auto msg = *res;
    if (messages.size() > 1)
    {
      // Multiple SEI messages in NALU, remove only the HDR10+ one
      buf.erase(std::next(buf.begin(), msg->m_msgOffset),
                std::next(buf.begin(), msg->m_payloadOffset + msg->m_payloadSize));
      HevcAddStartCodeEmulationPrevention3Byte(buf);
    }
    else
    {
      // Single SEI message in NALU
      buf.clear();
    }
  }
  else
  {
    // No HDR10+
    buf.clear();
  }

  return buf;
}

const std::vector<uint8_t> CHevcSei::RemoveCuvaFromSeiNalu(const uint8_t* inData, const size_t inDataLen)
{
  std::vector<uint8_t> buf;
  std::vector<CHevcSei> messages = CHevcSei::ParseSeiRbspUnclearedEmulation(inData, inDataLen, buf);

  if (auto res = CHevcSei::FindCuvaSeiMessage(buf, messages))
  {
    auto msg = *res;
    if (messages.size() > 1)
    {
      buf.erase(std::next(buf.begin(), msg->m_msgOffset),
                std::next(buf.begin(), msg->m_payloadOffset + msg->m_payloadSize));
      HevcAddStartCodeEmulationPrevention3Byte(buf);
    }
    else
    {
      buf.clear();
    }
  }
  else
  {
    buf.clear();
  }

  return buf;
}

std::optional<const CHevcSei*> CHevcSei::FindCuvaSeiMessage(
    std::vector<uint8_t>& buf, const std::vector<CHevcSei>& messages)
{
  for (const CHevcSei& sei : messages)
  {
    // User Data Registered ITU-T T.35
    if (sei.m_payloadType == 4 && sei.m_payloadSize >= 7)
    {
      CBitstreamReader br(buf.data() + sei.m_payloadOffset, sei.m_payloadSize);
      const auto itu_t_t35_country_code = br.ReadBits(8);
      const auto itu_t_t35_terminal_provider_code = br.ReadBits(16);
      const auto itu_t_t35_terminal_provider_oriented_code = br.ReadBits(16);

      // China, HDR VIVID
      if (itu_t_t35_country_code == 0x26 && itu_t_t35_terminal_provider_code == 0x0004)
      {
        return &sei;
      }
    }
  }

  return {};
}

bool CHevcSei::IsCuvaHdrVivid(const std::vector<CHevcSei>& messages,
                             std::vector<uint8_t>& buf)
{
  return FindCuvaSeiMessage(buf, messages).has_value();
}

const std::vector<uint8_t> CHevcSei::ConvertCuvaToHdr10(
    const uint8_t* inData, const size_t inDataLen)
{
  if (!inData || inDataLen == 0)
  {
    CLog::LogF(LOGWARNING, "HevcSei: Invalid input data for CUVA to HDR10 conversion");
    return {};
  }

  std::vector<uint8_t> buf;
  std::vector<CHevcSei> messages = CHevcSei::ParseSeiRbspUnclearedEmulation(inData, inDataLen, buf);

  // Get default HDR10 metadata
  MasteringDisplayColourVolume mdcv = GetDefaultMasteringDisplay();
  ContentLightLevel cll = GetDefaultContentLightLevel();

  // Check if there's existing mastering display and content light level information
  if (auto md = CHevcSei::ExtractMasteringDisplayColourVolume(messages, buf))
  {
    mdcv = md.value();
  }
  if (auto cl = CHevcSei::ExtractContentLightLevel(messages, buf))
  {
    cll = cl.value();
  }

  // Build HDR10 SEI messages
  std::vector<uint8_t> masteringSei = BuildMasteringDisplaySei(mdcv);
  std::vector<uint8_t> contentLightSei = BuildContentLightLevelSei(cll);

  CLog::LogF(LOGDEBUG, "HevcSei: Converting CUVA HDR VIVID to HDR10 metadata");

  // Process SEI payload: remove CUVA SEI and add HDR10 SEI
  std::vector<uint8_t> newRbsp;
  newRbsp.reserve(buf.size() + masteringSei.size() + contentLightSei.size() + 100);

  for (const CHevcSei& sei : messages)
  {
    // Check if this is a CUVA HDR VIVID SEI message
    bool isCuva = false;
    if (sei.m_payloadType == 4 && sei.m_payloadSize >= 7)
    {
      CBitstreamReader br(buf.data() + sei.m_msgOffset + (sei.m_payloadOffset - sei.m_msgOffset), sei.m_payloadSize);
      auto country = br.ReadBits(8);
      auto provider = br.ReadBits(16);
      if (country == 0x26 && provider == 0x0004)
      {
        isCuva = true;
      }
    }

    if (!isCuva)
    {
      // Keep non-CUVA SEI messages with boundary checking
      size_t msgStart = sei.m_msgOffset;
      size_t payloadDataSize = (sei.m_payloadOffset - sei.m_msgOffset) + sei.m_payloadSize;
      size_t msgEnd = msgStart + payloadDataSize;

      // Add boundary check to prevent buffer overflow
      if (msgStart < buf.size() && msgEnd <= buf.size())
      {
        newRbsp.insert(newRbsp.end(), buf.begin() + msgStart, buf.begin() + msgEnd);
      }
      else
      {
        CLog::LogF(LOGWARNING, "HevcSei: SEI message boundary out of range (start={}, end={}, buf={})", 
                   msgStart, msgEnd, buf.size());
      }
    }
    else {
      CLog::LogF(LOGDEBUG, "HevcSei: Removing CUVA HDR VIVID SEI message");
    }
  }

  // Append HDR10 SEI messages
  auto appendSei = [&](int payloadType, const std::vector<uint8_t>& data) {
    int type = payloadType;
    if (type < 255)
    {
      newRbsp.push_back(type & 0xFF);
    }
    else
    {
      int full = type;
      while (full >= 255)
      {
        newRbsp.push_back(0xFF);
        full -= 255;
      }
      newRbsp.push_back(full);
    }

    int size = static_cast<int>(data.size());
    int fullSize = size;
    while (fullSize >= 255)
    {
      newRbsp.push_back(0xFF);
      fullSize -= 255;
    }
    newRbsp.push_back(fullSize);

    newRbsp.insert(newRbsp.end(), data.begin(), data.end());
  };

  appendSei(137, masteringSei);   // mastering_display_colour_volume
  appendSei(144, contentLightSei); // content_light_level_info

  // Add emulation prevention 3 bytes
  HevcAddStartCodeEmulationPrevention3Byte(newRbsp);

  // CRITICAL FIX: Add rbsp_trailing_bits (0x80) as required by HEVC spec
  if (!newRbsp.empty())
  {
    if (newRbsp.back() != 0x80)
    {
      newRbsp.push_back(0x80); // rbsp_trailing_bits
    }
  }

  return newRbsp;
}
