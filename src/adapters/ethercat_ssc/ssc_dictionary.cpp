/* SPDX-License-Identifier: Apache-2.0 */

/* SSC 对象字典与 SDO 翻译：0x2100 配置、0x1600/0x1a00 映射、0x1c12/1c13 分配。
 * 对象索引/分界只引用生成的 pdo_schema 常量。 */

#include <cstring>

#include "adapters/ethercat_ssc/ssc_context.hpp"
#include "common/abi.hpp"
#include "domain/protocol/wire.hpp"

namespace ecat {
namespace {

const UCHAR kObjectName[] = "DTC1 gateway\0\xff";

UINT8 Abort(Error error) {
    switch (error) {
        case Error::None:
            return 0;
        case Error::State:
            return ABORTIDX_IN_THIS_STATE_DATA_CANNOT_BE_READ_OR_STORED;
        case Error::Capacity:
            return ABORTIDX_MAPPED_OBJECTS_EXCEED_PDO;
        case Error::Mapping:
            return ABORTIDX_OBJECT_CANT_BE_PDOMAPPED;
        default:
            return ABORTIDX_PARAM_IS_INCOMPATIBLE;
    }
}

bool Add(uint16_t index, unsigned maximum, unsigned bits, bool writable,
         bool array = false) {
    if (g_ctx.object_count == g_ctx.objects.size()) {
        return false;
    }
    auto& object = g_ctx.objects[g_ctx.object_count++];
    object.descriptor.Index = index;
    object.descriptor.ObjDesc = {
        static_cast<uint16_t>(
            array ? (bits == 16 ? DEFTYPE_UNSIGNED16 : DEFTYPE_UNSIGNED32)
                  : DEFTYPE_RECORD),
        static_cast<uint16_t>(maximum |
                              ((array ? OBJCODE_ARR : OBJCODE_REC) << 8))};
    object.entries =
        bits == 16 ? g_ctx.assignment_entries.data() : g_ctx.mapping_entries.data();
    if (!maximum) {
        object.entries = &g_ctx.empty_entry;
    }
    object.entries[0] = {
        DEFTYPE_UNSIGNED8, 8,
        static_cast<uint16_t>(ACCESS_READ |
                              (writable ? ACCESS_WRITE_PREOP : 0))};
    for (unsigned sub = 1; sub <= (array ? 1U : maximum); ++sub) {
        object.entries[sub] = {
            static_cast<uint16_t>(bits == 16 ? DEFTYPE_UNSIGNED16
                                             : DEFTYPE_UNSIGNED32),
            static_cast<uint16_t>(bits),
            static_cast<uint16_t>(ACCESS_READ |
                                  (writable ? ACCESS_WRITE_PREOP : 0))};
    }
    object.descriptor.pEntryDesc = maximum ? object.entries : &g_ctx.empty_entry;
    object.descriptor.pName = const_cast<UCHAR*>(kObjectName);
    object.descriptor.pVarPtr = &object.count;
    object.descriptor.Read = SscRead;
    object.descriptor.Write = writable ? SscWrite : nullptr;
    return COE_AddObjectToDic(&object.descriptor) == 0;
}

}  // namespace

SscObject* SscFind(uint16_t index) {
    for (unsigned i = 0; i < g_ctx.object_count; ++i) {
        if (g_ctx.objects[i].descriptor.Index == index) {
            return &g_ctx.objects[i];
        }
    }
    return nullptr;
}

UINT8 SscRead(UINT16 index, UINT8 subindex, UINT32 size, UINT16* data,
              UINT8 complete) {
    if (complete) {
        return ABORTIDX_COMPLETE_ACCESS_NOT_SUPPORTED;
    }
    auto* object = SscFind(index);
    if (!object || subindex > object->count) {
        return ABORTIDX_SUBINDEX_NOT_EXISTING;
    }
    auto* bytes = reinterpret_cast<uint8_t*>(data);
    if (subindex == 0) {
        if (size != 1) {
            return ABORTIDX_PARAM_LENGTH_ERROR;
        }
        bytes[0] = uint8_t(object->count);
        return 0;
    }
    if (index == 0x2100) {
        if (size != 4) {
            return ABORTIDX_PARAM_LENGTH_ERROR;
        }
        Put32(bytes, g_ctx.parameters[subindex - 1]);
        return 0;
    }
    if (index < 0x2000) {
        uint32_t value = 0;
        const Error error = g_ctx.layout->dictionary().Read(index, subindex, value);
        if (error != Error::None) {
            return Abort(error);
        }
        const unsigned expected = index >= 0x1c12 ? 2 : 4;
        if (size != expected) {
            return ABORTIDX_PARAM_LENGTH_ERROR;
        }
        if (expected == 2) {
            Put16(bytes, uint16_t(value));
        } else {
            Put32(bytes, value);
        }
        return 0;
    }
    const auto& prepared = g_ctx.layout->prepared();
    const auto& image =
        index < 0x3000 ? prepared.rx : prepared.tx;
    for (unsigned i = 0; i < image.count; ++i) {
        const auto& entry = image.entries[i];
        if (entry.index == index && entry.subindex == subindex) {
            if (size != entry.bits / 8U) {
                return ABORTIDX_PARAM_LENGTH_ERROR;
            }
            const auto& buffer = index < 0x3000 ? g_ctx.rx_image : g_ctx.tx_image;
            std::memcpy(bytes, buffer.data() + entry.offset, size);
            return 0;
        }
    }
    return ABORTIDX_SUBINDEX_NOT_EXISTING;
}

UINT8 SscWrite(UINT16 index, UINT8 subindex, UINT32 size, UINT16* data,
               UINT8 complete) {
    if (complete) {
        return ABORTIDX_COMPLETE_ACCESS_NOT_SUPPORTED;
    }
    auto* session = g_ctx.session;
    if ((nAlStatus & STATE_MASK) != STATE_PREOP || !session ||
        session->state() != BusinessState::Unconfigured) {
        return ABORTIDX_IN_THIS_STATE_DATA_CANNOT_BE_READ_OR_STORED;
    }
    if (size != 1 && size != 2 && size != 4) {
        return ABORTIDX_PARAM_LENGTH_ERROR;
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(data);
    const uint32_t value = size == 4   ? U32(bytes)
                           : size == 2 ? U16(bytes)
                                       : bytes[0];
    Error error;
    if (index == 0x2100) {
        if (size != 4 || !subindex || subindex > 22) {
            return ABORTIDX_PARAM_LENGTH_ERROR;
        }
        error = g_ctx.layout->transaction().Write(subindex, value);
        if (error == Error::None) {
            g_ctx.parameters[subindex - 1] = value;
            g_ctx.mapping_ready = false;
        }
        return Abort(error);
    }
    error = g_ctx.layout->dictionary().Write(index, subindex, uint8_t(size),
                                             value);
    if (error == Error::None && subindex == 0) {
        SscFind(index)->count = uint16_t(value);
    }
    if (error == Error::None) {
        g_ctx.mapping_ready = false;
    }
    return Abort(error);
}

bool SscDescribe(const ImageLayout& image, bool tx) {
    for (unsigned i = 0; i < image.count; ++i) {
        const auto& entry = image.entries[i];
        auto* object = SscFind(entry.index);
        if (!object) {
            return false;
        }
        if (entry.subindex == 1) {
            unsigned count = 1;
            while (i + count < image.count &&
                   image.entries[i + count].index == entry.index) {
                ++count;
            }
            if (g_ctx.data_entry_count + count + 1 > g_ctx.data_entries.size()) {
                return false;
            }
            object->entries =
                g_ctx.data_entries.data() + g_ctx.data_entry_count;
            g_ctx.data_entry_count += count + 1;
            object->entries[0] = g_ctx.empty_entry;
            object->descriptor.pEntryDesc = object->entries;
        }
        object->count = entry.subindex;
        object->descriptor.ObjDesc.ObjFlags =
            static_cast<uint16_t>(entry.subindex | (OBJCODE_REC << 8));
        const unsigned type = entry.bits == 8    ? DEFTYPE_UNSIGNED8
                              : entry.bits == 16 ? DEFTYPE_UNSIGNED16
                              : entry.bits == 32 ? DEFTYPE_UNSIGNED32
                              : entry.bits == 64 ? DEFTYPE_UNSIGNED64
                                                 : DEFTYPE_OCTETSTRING;
        object->entries[entry.subindex] = {
            static_cast<uint16_t>(type), entry.bits,
            static_cast<uint16_t>(ACCESS_READ | (tx ? OBJACCESS_TXPDOMAPPING
                                                    : OBJACCESS_RXPDOMAPPING))};
    }
    return true;
}

bool SscRegisterObjects() {
    g_ctx.object_count = 0;
    std::memset(static_cast<void*>(g_ctx.objects.data()), 0,
                sizeof(g_ctx.objects));
    for (unsigned i = 0; i < 5; ++i) {
        if (!Add(0x1600 + i, 255, 32, true) ||
            !Add(0x1a00 + i, 255, 32, true)) {
            return false;
        }
    }
    if (!Add(0x1c12, 5, 16, true, true) || !Add(0x1c13, 5, 16, true, true) ||
        !Add(0x2100, 22, 32, true, true)) {
        return false;
    }
    SscFind(0x2100)->count = 22;
    for (uint16_t index :
         {0x2000, 0x2010, 0x2011, 0x2020, 0x2021, 0x2022, 0x2023, 0x2024,
          0x2025, 0x3000, 0x3010, 0x3011, 0x3012, 0x3013, 0x3020, 0x3021,
          0x3022, 0x3023, 0x3024, 0x3025, 0x3030, 0x3040}) {
        if (!Add(index, 0, 0, false)) {
            return false;
        }
    }
    sSyncManOutPar.u16SyncTypesSupported = SYNCTYPE_FREERUNSUPP;
    sSyncManInPar.u16SyncTypesSupported = SYNCTYPE_FREERUNSUPP;
    sSyncManOutPar.u16SyncTypesSupported |= SYNCTYPE_SYNCHRONSUPP;
    sSyncManInPar.u16SyncTypesSupported |= SYNCTYPE_SYNCHRONSUPP;
    if (!Add(0x2050,0,32,false) || !Add(0x3051,0,32,false)) return false;
    return true;
}

}  // namespace ecat
