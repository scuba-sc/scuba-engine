#pragma once
#include "assets/formats.h"

namespace fixture {
using scuba::assets::Bytes;
using scuba::assets::Put32;
inline void Set32(Bytes& bytes, size_t offset, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.at(offset + shift / 8) = static_cast<uint8_t>((value >> shift) & 255U);
}
inline void Set16(Bytes& bytes, size_t offset, uint16_t value) {
    bytes.at(offset) = static_cast<uint8_t>(value & 255U);
    bytes.at(offset + 1) = static_cast<uint8_t>(value >> 8);
}
inline Bytes Cry() {
    Bytes mesh(260, 0);
    Set32(mesh, 8, 3); Set32(mesh, 12, 3);
    Set32(mesh, 28, 2); Set32(mesh, 48, 3);
    Bytes positions(24, 0);
    Set32(positions, 4, 0); Set32(positions, 8, 3); Set32(positions, 12, 12);
    for (float value : {0.F,0.F,0.F, 1.F,0.F,0.F, 0.F,1.F,0.F}) Put32(positions, std::bit_cast<uint32_t>(value));
    Bytes indices(30, 0);
    Set32(indices, 4, 5); Set32(indices, 8, 3); Set32(indices, 12, 2);
    Set16(indices, 24, 0); Set16(indices, 26, 1); Set16(indices, 28, 2);
    Bytes output(64, 0);
    Set32(output, 0, 0x68437243); Set32(output, 4, 0x746); Set32(output, 8, 3); Set32(output, 12, 16);
    size_t entry = 16;
    uint32_t id = 1;
    for (const auto& chunk : {mesh, positions, indices}) {
        Set16(output, entry, id == 1 ? uint16_t{0x1000} : uint16_t{0x1016});
        Set16(output, entry + 2, 0x800);
        Set32(output, entry + 4, id++);
        Set32(output, entry + 8, scuba::assets::Size32(chunk.size()));
        Set32(output, entry + 12, scuba::assets::Size32(output.size()));
        output.insert(output.end(), chunk.begin(), chunk.end());
        entry += 16;
    }
    return output;
}
inline Bytes Dds(uint32_t dxgi = 0) {
    const bool smallBlock = dxgi == 0 || dxgi == 71 || dxgi == 72 || dxgi == 80 || dxgi == 81;
    Bytes bytes((dxgi == 0 ? 128U : 148U) + (smallBlock ? 8U : 16U), 0);
    Set32(bytes, 0, 0x20534444); Set32(bytes, 4, 124);
    Set32(bytes, 8, 0x81007); Set32(bytes, 12, 4); Set32(bytes, 16, 4);
    Set32(bytes, 20, smallBlock ? 8U : 16U); Set32(bytes, 28, 1);
    Set32(bytes, 76, 32); Set32(bytes, 80, 4);
    Set32(bytes, 84, dxgi == 0 ? 0x31545844U : 0x30315844U); Set32(bytes, 108, 0x1000);
    if (dxgi != 0) { Set32(bytes, 128, dxgi); Set32(bytes, 132, 3); Set32(bytes, 140, 1); }
    else { Set16(bytes, 128, 0xF800); Set16(bytes, 130, 0x07E0); }
    return bytes;
}
inline Bytes Dcb() {
    Bytes names;
    auto name = [&](std::string_view value) {
        const auto offset = scuba::assets::Size32(names.size());
        names.insert(names.end(), value.begin(), value.end()); names.push_back(0);
        return offset;
    };
    const auto type = name("Ship"), speed = name("speed"), target = name("target");
    const auto recordA = name("TriangleA"), recordB = name("TriangleB");
    auto values = scuba::assets::Binary("libs/foundry/records/synthetic.xml"); values.push_back(0);
    Bytes bytes(120, 0);
    Set32(bytes, 4, 6); Set32(bytes, 16, 1); Set32(bytes, 20, 2);
    Set32(bytes, 28, 1); Set32(bytes, 32, 2);
    Set32(bytes, 112, scuba::assets::Size32(values.size())); Set32(bytes, 116, scuba::assets::Size32(names.size()));
    const size_t structs = bytes.size(); bytes.resize(bytes.size() + 16);
    Set32(bytes, structs, type); Set32(bytes, structs + 4, UINT32_MAX); Set16(bytes, structs + 8, 2);
    for (const auto property : {speed, target}) {
        const size_t start = bytes.size(); bytes.resize(start + 12);
        Set32(bytes, start, property);
        Set16(bytes, start + 6, property == speed ? uint16_t{0xB} : uint16_t{0x310});
    }
    Put32(bytes, 2); Put32(bytes, 0);
    for (uint8_t record = 0; record < 2; ++record) {
        const size_t start = bytes.size(); bytes.resize(start + 32);
        Set32(bytes, start, record == 0 ? recordA : recordB);
        bytes[start + 12] = static_cast<uint8_t>(record + 1);
        Set16(bytes, start + 28, record); Set16(bytes, start + 30, 24);
    }
    bytes.insert(bytes.end(), values.begin(), values.end()); bytes.insert(bytes.end(), names.begin(), names.end());
    for (uint8_t record = 0; record < 2; ++record) {
        Put32(bytes, std::bit_cast<uint32_t>(record == 0 ? 10.F : 20.F));
        Put32(bytes, record == 0 ? 1U : 0U);
        const size_t start = bytes.size(); bytes.resize(start + 16);
        bytes[start] = record == 0 ? uint8_t{2} : uint8_t{1};
    }
    return bytes;
}
} // namespace fixture
