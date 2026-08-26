// -*- mode: c++ -*-
// Copyright 2009-2026 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S. Government retains certain rights in this software.
//
// Copyright (c) 2009-2026, NTESS
// All rights reserved.

#include "collectiveServiceData.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace SST::Collective {
namespace {

void appendU16(std::vector<uint8_t>& bytes, uint16_t value)
{
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value));
}

void appendU32(std::vector<uint8_t>& bytes, uint32_t value)
{
    for ( int shift = 24; shift >= 0; shift -= 8 ) bytes.push_back(static_cast<uint8_t>(value >> shift));
}

void appendU64(std::vector<uint8_t>& bytes, uint64_t value)
{
    for ( int shift = 56; shift >= 0; shift -= 8 ) bytes.push_back(static_cast<uint8_t>(value >> shift));
}

uint16_t readU16(const uint8_t*& bytes)
{
    const uint16_t value = (uint16_t { bytes[0] } << 8) | uint16_t { bytes[1] };
    bytes += 2;
    return value;
}

uint32_t readU32(const uint8_t*& bytes)
{
    uint32_t value = 0;
    for ( int index = 0; index < 4; ++index ) value = (value << 8) | bytes[index];
    bytes += 4;
    return value;
}

uint64_t readU64(const uint8_t*& bytes)
{
    uint64_t value = 0;
    for ( int index = 0; index < 8; ++index ) value = (value << 8) | bytes[index];
    bytes += 8;
    return value;
}

} // namespace

CollectiveServiceData::CollectiveServiceData(
    CollectiveDescriptorFieldsV1 fields, std::vector<uint8_t> owned_bytes) :
    fields(std::move(fields)),
    owned_bytes(std::move(owned_bytes))
{}

CollectiveServiceData::~CollectiveServiceData() = default;

CollectiveServiceData* CollectiveServiceData::clone() const
{
    if ( validateIntrinsic() != DescriptorValidation::Valid ) {
        throw std::logic_error("Cannot clone malformed collective service data");
    }
    return new CollectiveServiceData(*this);
}

DescriptorValidation CollectiveServiceData::validateIntrinsic() const
{
    if ( fields.service_schema_version != COLLECTIVE_SERVICE_SCHEMA_V1 ) {
        return DescriptorValidation::InvalidServiceSchema;
    }
    if ( fields.wire_format_version != COLLECTIVE_WIRE_FORMAT_V1 ) {
        return DescriptorValidation::InvalidWireFormat;
    }
    if ( !fields.route.valid() ) return DescriptorValidation::InvalidNamespace;
    if ( fields.total_chunks == 0 ) return DescriptorValidation::InvalidChunkCount;
    if ( fields.chunk_index >= fields.total_chunks ) return DescriptorValidation::InvalidChunkIndex;
    if ( fields.element_count == 0 || fields.total_elements == 0 ) {
        return DescriptorValidation::InvalidElementCount;
    }
    if ( fields.element_offset > std::numeric_limits<uint64_t>::max() - fields.element_count ||
         fields.element_offset + fields.element_count > fields.total_elements ) {
        return DescriptorValidation::InvalidElementRange;
    }
    if ( !isValid(fields.operation) ) return DescriptorValidation::InvalidOperation;
    if ( !isValid(fields.datatype) ) return DescriptorValidation::InvalidDatatype;
    if ( fields.arithmetic_policy != COLLECTIVE_ARITHMETIC_POLICY_V1 ) {
        return DescriptorValidation::InvalidArithmeticPolicy;
    }
    if ( !isValid(fields.direction) ) return DescriptorValidation::InvalidDirection;

    const uint64_t element_bytes = datatypeSize(fields.datatype);
    if ( fields.element_count > std::numeric_limits<uint64_t>::max() / element_bytes ) {
        return DescriptorValidation::PayloadSizeOverflow;
    }
    if ( fields.logical_payload_bytes != fields.element_count * element_bytes ) {
        return DescriptorValidation::PayloadSizeMismatch;
    }
    if ( fields.data_present > 1 ) return DescriptorValidation::InvalidDataPresent;
    if ( fields.data_present == 0 ? !owned_bytes.empty() : owned_bytes.size() != fields.logical_payload_bytes ) {
        return DescriptorValidation::OwnedSizeMismatch;
    }
    if ( owned_bytes.size() > MAX_OWNED_BYTES ) return DescriptorValidation::EncodedLengthTooLarge;
    if ( fields.modeled_wire_bytes < fields.logical_payload_bytes ) {
        return DescriptorValidation::InvalidModeledSize;
    }
    return DescriptorValidation::Valid;
}

std::vector<uint8_t> CollectiveServiceData::canonicalBytes() const
{
    if ( owned_bytes.size() > MAX_OWNED_BYTES ) {
        throw std::length_error("Collective service payload exceeds the V1 canonical limit");
    }

    std::vector<uint8_t> bytes;
    bytes.reserve(FIXED_PREFIX_BYTES + owned_bytes.size());
    appendU16(bytes, fields.service_schema_version);
    appendU16(bytes, fields.wire_format_version);
    appendU64(bytes, fields.route.job_namespace);
    appendU64(bytes, fields.route.route_id);
    appendU64(bytes, fields.invocation_id);
    appendU32(bytes, fields.chunk_index);
    appendU32(bytes, fields.total_chunks);
    appendU64(bytes, fields.element_offset);
    appendU64(bytes, fields.element_count);
    appendU64(bytes, fields.total_elements);
    bytes.push_back(static_cast<uint8_t>(fields.operation));
    bytes.push_back(static_cast<uint8_t>(fields.datatype));
    appendU16(bytes, fields.arithmetic_policy);
    bytes.push_back(static_cast<uint8_t>(fields.direction));
    appendU64(bytes, fields.logical_payload_bytes);
    appendU64(bytes, fields.modeled_wire_bytes);
    bytes.push_back(fields.data_present);
    appendU64(bytes, static_cast<uint64_t>(owned_bytes.size()));
    bytes.insert(bytes.end(), owned_bytes.begin(), owned_bytes.end());
    return bytes;
}

DescriptorValidation CollectiveServiceData::decodeCanonical(
    const uint8_t* bytes, uint64_t length, CollectiveServiceData& output)
{
    if ( bytes == nullptr || length < FIXED_PREFIX_BYTES ) return DescriptorValidation::EncodedLengthMismatch;
    if ( length > FIXED_PREFIX_BYTES + MAX_OWNED_BYTES ) return DescriptorValidation::EncodedLengthTooLarge;

    const uint8_t* cursor = bytes;
    CollectiveDescriptorFieldsV1 fields;
    fields.service_schema_version = readU16(cursor);
    fields.wire_format_version    = readU16(cursor);
    fields.route.job_namespace    = readU64(cursor);
    fields.route.route_id         = readU64(cursor);
    fields.invocation_id          = readU64(cursor);
    fields.chunk_index            = readU32(cursor);
    fields.total_chunks           = readU32(cursor);
    fields.element_offset         = readU64(cursor);
    fields.element_count          = readU64(cursor);
    fields.total_elements         = readU64(cursor);
    fields.operation              = static_cast<CollectiveOperation>(*cursor++);
    fields.datatype               = static_cast<CollectiveDatatype>(*cursor++);
    fields.arithmetic_policy      = readU16(cursor);
    fields.direction              = static_cast<CollectiveDirection>(*cursor++);
    fields.logical_payload_bytes  = readU64(cursor);
    fields.modeled_wire_bytes     = readU64(cursor);
    fields.data_present           = *cursor++;
    const uint64_t owned_length   = readU64(cursor);

    if ( owned_length > MAX_OWNED_BYTES ) return DescriptorValidation::EncodedLengthTooLarge;
    if ( owned_length != length - FIXED_PREFIX_BYTES ) return DescriptorValidation::EncodedLengthMismatch;

    CollectiveServiceData decoded(fields,
        std::vector<uint8_t>(bytes + FIXED_PREFIX_BYTES, bytes + FIXED_PREFIX_BYTES + owned_length));
    const DescriptorValidation status = decoded.validateIntrinsic();
    if ( status != DescriptorValidation::Valid ) return status;

    output.fields      = decoded.fields;
    output.owned_bytes = std::move(decoded.owned_bytes);
    return DescriptorValidation::Valid;
}

void CollectiveServiceData::serialize_order(SST::Core::Serialization::serializer& ser)
{
    uint64_t             blob_size = 0;
    std::vector<uint8_t> blob;

    if ( ser.mode() != SST::Core::Serialization::serializer::UNPACK ) {
        if ( validateIntrinsic() != DescriptorValidation::Valid ) {
            throw std::logic_error("Cannot serialize malformed collective service data");
        }
        blob      = canonicalBytes();
        blob_size = static_cast<uint64_t>(blob.size());
    }

    SST_SER(blob_size);

    if ( ser.mode() == SST::Core::Serialization::serializer::UNPACK ) {
        if ( blob_size < FIXED_PREFIX_BYTES || blob_size > FIXED_PREFIX_BYTES + MAX_OWNED_BYTES ) {
            throw std::length_error("Invalid serialized collective service-data length");
        }
        blob.resize(static_cast<size_t>(blob_size));
    }

    for ( uint8_t& byte : blob ) SST_SER(byte);

    if ( ser.mode() == SST::Core::Serialization::serializer::UNPACK ) {
        CollectiveServiceData decoded;
        if ( decodeCanonical(blob.data(), blob_size, decoded) != DescriptorValidation::Valid ) {
            throw std::runtime_error("Malformed serialized collective service data");
        }
        fields      = decoded.fields;
        owned_bytes = std::move(decoded.owned_bytes);
    }
}

} // namespace SST::Collective
