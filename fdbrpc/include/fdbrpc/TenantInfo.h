/*
 * TenantInfo.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
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

#pragma once
#ifndef FDBRPC_TENANTINFO_H_
#define FDBRPC_TENANTINFO_H_
#include "fdbrpc/TenantName.h"
#include "fdbrpc/TokenSign.h"
#include "fdbrpc/TokenCache.h"
#include "fdbrpc/FlowTransport.h"
#include "flow/Arena.h"
#include "flow/Knobs.h"
#include "flow/WipedString.h"

struct TenantInfo {
	static constexpr const int64_t INVALID_TENANT = -1;

	Arena arena;
	int64_t tenantId;
	Optional<StringRef> prefix;
	Optional<WipedString> token;
	// this field is not serialized and instead set by FlowTransport during
	// deserialization. This field indicates whether the client is trusted.
	// Untrusted clients are generally expected to set a tenant ID
	bool trusted = false;
	// Is set during deserialization. It will be set to true if the tenant
	// is set and the client is authorized to use this tenant.
	bool tenantAuthorized = false;

	// Helper function for most endpoints that read/write data. This returns true iff
	// the client is either a) a trusted peer or b) is accessing keyspace belonging to a tenant,
	// for which it has a valid authorization token.
	// NOTE: In a cluster where TenantMode is OPTIONAL or DISABLED, tenant name may be unset.
	//       In such case, the request containing such TenantInfo is valid iff the requesting peer is trusted.
	bool isAuthorized() const { return trusted || tenantAuthorized; }
	bool hasTenant() const { return tenantId != INVALID_TENANT; }

	TenantInfo() : tenantId(INVALID_TENANT) {}
	TenantInfo(int64_t tenantId, Optional<WipedString> const& token) : tenantId(tenantId), token(token) {
		if (tenantId != INVALID_TENANT) {
			prefix = idToPrefix(tenantId, arena);
		}
	}

	static StringRef idToPrefix(int64_t id, Arena& arena) {
		int64_t swapped = bigEndian64(id);
		return StringRef(arena, reinterpret_cast<const uint8_t*>(&swapped), sizeof(id));
	}
};

template <>
struct serializable_traits<TenantInfo> : std::true_type {
	template <class Archiver>
	static void serialize(Archiver& ar, TenantInfo& v) {
		serializer(ar, v.tenantId, v.token, v.arena);
		if constexpr (Archiver::isDeserializing) {
			bool tenantAuthorized = FLOW_KNOBS->ALLOW_TOKENLESS_TENANT_ACCESS;
			if (!tenantAuthorized && v.tenantId != TenantInfo::INVALID_TENANT && v.token.present()) {
				tenantAuthorized = TokenCache::instance().validate(v.tenantId, v.token.get());
			}
			v.trusted = FlowTransport::transport().currentDeliveryPeerIsTrusted();
			v.tenantAuthorized = tenantAuthorized;
			if (v.hasTenant()) {
				v.prefix = TenantInfo::idToPrefix(v.tenantId, v.arena);
			}
		}
	}
};

#endif // FDBRPC_TENANTINFO_H_
