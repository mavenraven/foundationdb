/*
 * MetaclusterRestoreWorkload.actor.cpp
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

#include <cstdint>
#include <limits>
#include "fdbclient/BackupAgent.actor.h"
#include "fdbclient/ClusterConnectionMemoryRecord.h"
#include "fdbclient/FDBOptions.g.h"
#include "fdbclient/Metacluster.h"
#include "fdbclient/MetaclusterManagement.actor.h"
#include "fdbclient/ReadYourWrites.h"
#include "fdbclient/RunTransaction.actor.h"
#include "fdbclient/ThreadSafeTransaction.h"
#include "fdbrpc/simulator.h"
#include "fdbserver/workloads/MetaclusterConsistency.actor.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "fdbserver/Knobs.h"
#include "flow/Error.h"
#include "flow/IRandom.h"
#include "flow/ThreadHelper.actor.h"
#include "flow/flow.h"
#include "flow/actorcompiler.h" // This must be the last #include.

struct MetaclusterRestoreWorkload : TestWorkload {
	static constexpr auto NAME = "MetaclusterRestore";

	struct DataClusterData {
		Database db;
		std::set<int64_t> tenants;
		std::set<TenantGroupName> tenantGroups;
		bool restored = false;
		bool restoreHasMessages = false;

		DataClusterData() {}
		DataClusterData(Database db) : db(db) {}
	};

	struct TenantData {
		enum class AccessTime { NONE, BEFORE_BACKUP, DURING_BACKUP, AFTER_BACKUP };

		TenantName name;
		ClusterName cluster;
		Optional<TenantGroupName> tenantGroup;
		AccessTime createTime = AccessTime::BEFORE_BACKUP;
		AccessTime renameTime = AccessTime::NONE;
		AccessTime configureTime = AccessTime::NONE;

		TenantData() {}
		TenantData(TenantName name, ClusterName cluster, Optional<TenantGroupName> tenantGroup, AccessTime createTime)
		  : name(name), cluster(cluster), tenantGroup(tenantGroup), createTime(createTime) {}
	};

	struct TenantGroupData {
		ClusterName cluster;
		std::set<int64_t> tenants;
	};

	Reference<IDatabase> managementDb;
	std::map<ClusterName, DataClusterData> dataDbs;
	std::vector<ClusterName> dataDbIndex;

	std::map<int64_t, TenantData> createdTenants;
	std::map<TenantName, int64_t> tenantNameIndex;
	std::map<TenantGroupName, TenantGroupData> tenantGroups;

	std::set<int64_t> deletedTenants;
	std::vector<std::pair<int64_t, TenantMapEntry>> managementTenantsBeforeRestore;

	int initialTenants;
	int maxTenants;
	int maxTenantGroups;
	int tenantGroupCapacity;

	bool recoverManagementCluster;
	bool recoverDataClusters;

	bool backupComplete = false;
	double endTime = std::numeric_limits<double>::max();

	MetaclusterRestoreWorkload(WorkloadContext const& wcx) : TestWorkload(wcx) {
		maxTenants = std::min<int>(1e8 - 1, getOption(options, "maxTenants"_sr, 1000));
		initialTenants = std::min<int>(maxTenants, getOption(options, "initialTenants"_sr, 40));
		maxTenantGroups = std::min<int>(2 * maxTenants, getOption(options, "maxTenantGroups"_sr, 20));

		tenantGroupCapacity = (initialTenants / 2 + maxTenantGroups - 1) / g_simulator->extraDatabases.size();
		int mode = deterministicRandom()->randomInt(0, 3);
		recoverManagementCluster = (mode != 2);
		recoverDataClusters = (mode != 1);
	}

	ClusterName chooseClusterName() { return dataDbIndex[deterministicRandom()->randomInt(0, dataDbIndex.size())]; }

	TenantName chooseTenantName() {
		TenantName tenant(format("tenant%08d", deterministicRandom()->randomInt(0, maxTenants)));
		return tenant;
	}

	Optional<TenantGroupName> chooseTenantGroup(Optional<ClusterName> cluster = Optional<ClusterName>()) {
		Optional<TenantGroupName> tenantGroup;
		if (deterministicRandom()->coinflip()) {
			if (!cluster.present()) {
				tenantGroup =
				    TenantGroupNameRef(format("tenantgroup%08d", deterministicRandom()->randomInt(0, maxTenantGroups)));
			} else {
				auto const& existingGroups = dataDbs[cluster.get()].tenantGroups;
				if (deterministicRandom()->coinflip() && !existingGroups.empty()) {
					tenantGroup = deterministicRandom()->randomChoice(
					    std::vector<TenantGroupName>(existingGroups.begin(), existingGroups.end()));
				} else if (tenantGroups.size() < maxTenantGroups) {
					do {
						tenantGroup = TenantGroupNameRef(
						    format("tenantgroup%08d", deterministicRandom()->randomInt(0, maxTenantGroups)));
					} while (tenantGroups.count(tenantGroup.get()) > 0);
				}
			}
		}

		return tenantGroup;
	}

	// Used to gradually increase capacity so that the tenants are somewhat evenly distributed across the clusters
	ACTOR static Future<Void> increaseMetaclusterCapacity(MetaclusterRestoreWorkload* self) {
		self->tenantGroupCapacity = ceil(self->tenantGroupCapacity * 1.2);
		state Reference<ITransaction> tr = self->managementDb->createTransaction();
		loop {
			try {
				tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
				state int dbIndex;
				for (dbIndex = 0; dbIndex < self->dataDbIndex.size(); ++dbIndex) {
					DataClusterMetadata clusterMetadata =
					    wait(MetaclusterAPI::getClusterTransaction(tr, self->dataDbIndex[dbIndex]));
					DataClusterEntry updatedEntry = clusterMetadata.entry;
					updatedEntry.capacity.numTenantGroups = self->tenantGroupCapacity;
					MetaclusterAPI::updateClusterMetadata(
					    tr, self->dataDbIndex[dbIndex], clusterMetadata, {}, updatedEntry);
				}
				wait(safeThreadFutureToFuture(tr->commit()));
				break;
			} catch (Error& e) {
				wait(safeThreadFutureToFuture(tr->onError(e)));
			}
		}

		return Void();
	}

	Future<Void> setup(Database const& cx) override {
		if (clientId == 0) {
			return _setup(cx, this);
		} else {
			return Void();
		}
	}
	ACTOR static Future<Void> _setup(Database cx, MetaclusterRestoreWorkload* self) {
		Reference<IDatabase> threadSafeHandle =
		    wait(unsafeThreadFutureToFuture(ThreadSafeDatabase::createFromExistingDatabase(cx)));

		MultiVersionApi::api->selectApiVersion(cx->apiVersion.version());
		self->managementDb = MultiVersionDatabase::debugCreateFromExistingDatabase(threadSafeHandle);
		wait(success(MetaclusterAPI::createMetacluster(
		    self->managementDb,
		    "management_cluster"_sr,
		    deterministicRandom()->randomInt(TenantAPI::TENANT_ID_PREFIX_MIN_VALUE,
		                                     TenantAPI::TENANT_ID_PREFIX_MAX_VALUE + 1))));

		ASSERT(g_simulator->extraDatabases.size() > 0);
		state std::vector<std::string>::iterator extraDatabasesItr;
		for (extraDatabasesItr = g_simulator->extraDatabases.begin();
		     extraDatabasesItr != g_simulator->extraDatabases.end();
		     ++extraDatabasesItr) {
			ClusterConnectionString ccs(*extraDatabasesItr);
			auto extraFile = makeReference<ClusterConnectionMemoryRecord>(ccs);
			state ClusterName clusterName = ClusterName(format("cluster_%08d", self->dataDbs.size()));
			Database db = Database::createDatabase(extraFile, ApiVersion::LATEST_VERSION);
			self->dataDbIndex.push_back(clusterName);
			self->dataDbs[clusterName] = DataClusterData(db);

			DataClusterEntry clusterEntry;
			clusterEntry.capacity.numTenantGroups = self->tenantGroupCapacity;

			wait(MetaclusterAPI::registerCluster(self->managementDb, clusterName, ccs, clusterEntry));
		}

		TraceEvent(SevDebug, "MetaclusterRestoreWorkloadCreateTenants").detail("NumTenants", self->initialTenants);

		while (self->createdTenants.size() < self->initialTenants) {
			wait(createTenant(self, TenantData::AccessTime::BEFORE_BACKUP));
		}

		TraceEvent(SevDebug, "MetaclusterRestoreWorkloadCreateTenantsComplete");

		return Void();
	}

	ACTOR static Future<std::string> backupCluster(ClusterName clusterName,
	                                               Database dataDb,
	                                               MetaclusterRestoreWorkload* self) {
		state FileBackupAgent backupAgent;
		state Standalone<StringRef> backupContainer = "file://simfdb/backups/"_sr.withSuffix(clusterName);
		state Standalone<VectorRef<KeyRangeRef>> backupRanges;

		addDefaultBackupRanges(backupRanges);

		TraceEvent("MetaclusterRestoreWorkloadSubmitBackup").detail("ClusterName", clusterName);
		try {
			wait(backupAgent.submitBackup(
			    dataDb, backupContainer, {}, 0, 0, clusterName.toString(), backupRanges, StopWhenDone::True));
		} catch (Error& e) {
			if (e.code() != error_code_backup_unneeded && e.code() != error_code_backup_duplicate)
				throw;
		}

		TraceEvent("MetaclusterRestoreWorkloadWaitBackup").detail("ClusterName", clusterName);
		state Reference<IBackupContainer> container;
		wait(success(backupAgent.waitBackup(dataDb, clusterName.toString(), StopWhenDone::True, &container)));
		TraceEvent("MetaclusterRestoreWorkloadBackupComplete").detail("ClusterName", clusterName);
		return container->getURL();
	}

	ACTOR static Future<Void> restoreDataCluster(ClusterName clusterName,
	                                             Database dataDb,
	                                             std::string backupUrl,
	                                             bool addToMetacluster,
	                                             ForceJoinNewMetacluster forceJoinNewMetacluster,
	                                             MetaclusterRestoreWorkload* self) {
		state FileBackupAgent backupAgent;
		state Standalone<VectorRef<KeyRangeRef>> backupRanges;
		addDefaultBackupRanges(backupRanges);

		TraceEvent("MetaclusterRestoreWorkloadClearDatabase").detail("ClusterName", clusterName);
		wait(runTransaction(dataDb.getReference(),
		                    [backupRanges = backupRanges](Reference<ReadYourWritesTransaction> tr) {
			                    tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			                    for (auto range : backupRanges) {
				                    tr->clear(range);
			                    }
			                    return Future<Void>(Void());
		                    }));

		TraceEvent("MetaclusterRestoreWorkloadRestoreCluster").detail("ClusterName", clusterName);
		wait(success(backupAgent.restore(dataDb, dataDb, clusterName, StringRef(backupUrl), {}, backupRanges)));

		state std::vector<std::string> messages;
		if (addToMetacluster) {
			TraceEvent("MetaclusterRestoreWorkloadAddClusterToMetacluster").detail("ClusterName", clusterName);
			if (deterministicRandom()->coinflip()) {
				TraceEvent("MetaclusterRestoreWorkloadAddClusterToMetaclusterDryRun")
				    .detail("ClusterName", clusterName);
				wait(MetaclusterAPI::restoreCluster(self->managementDb,
				                                    clusterName,
				                                    dataDb->getConnectionRecord()->getConnectionString(),
				                                    ApplyManagementClusterUpdates::True,
				                                    RestoreDryRun::True,
				                                    forceJoinNewMetacluster,
				                                    &messages));
				TraceEvent("MetaclusterRestoreWorkloadAddClusterToMetaclusterDryRunDone")
				    .detail("ClusterName", clusterName);
				messages.clear();
			}

			wait(MetaclusterAPI::restoreCluster(self->managementDb,
			                                    clusterName,
			                                    dataDb->getConnectionRecord()->getConnectionString(),
			                                    ApplyManagementClusterUpdates::True,
			                                    RestoreDryRun::False,
			                                    forceJoinNewMetacluster,
			                                    &messages));
			TraceEvent("MetaclusterRestoreWorkloadRestoreComplete").detail("ClusterName", clusterName);
		}

		self->dataDbs[clusterName].restored = true;
		self->dataDbs[clusterName].restoreHasMessages = !messages.empty();

		return Void();
	}

	void removeTrackedTenant(int64_t tenantId) {
		auto itr = createdTenants.find(tenantId);
		if (itr != createdTenants.end()) {
			TraceEvent(SevDebug, "MetaclusterRestoreWorkloadRemoveTrackedTenant")
			    .detail("TenantId", tenantId)
			    .detail("TenantName", itr->second.name);
			deletedTenants.insert(tenantId);
			dataDbs[itr->second.cluster].tenants.erase(tenantId);
			if (itr->second.tenantGroup.present()) {
				tenantGroups[itr->second.tenantGroup.get()].tenants.erase(tenantId);
			}
			createdTenants.erase(itr);
		}
	}

	// A map from tenant name to a pair of IDs. The first ID is from the data cluster, and the second is from the
	// management cluster.
	using TenantCollisions = std::unordered_map<TenantName, std::pair<int64_t, int64_t>>;

	using GroupCollisions = std::unordered_set<TenantGroupName>;

	Future<Void> resolveTenantCollisions(MetaclusterRestoreWorkload* self,
	                                     ClusterName clusterName,
	                                     Database dataDb,
	                                     TenantCollisions const& tenantCollisions) {
		TraceEvent("MetaclusterRestoreWorkloadDeleteTenantCollisions")
		    .detail("FromCluster", clusterName)
		    .detail("TenantCollisions", tenantCollisions.size());
		std::vector<Future<Void>> deleteFutures;
		for (auto const& t : tenantCollisions) {
			// If the data cluster tenant is expected, then remove the management tenant
			// Note that the management tenant may also have been expected
			if (self->createdTenants.count(t.second.first)) {
				removeTrackedTenant(t.second.second);
				deleteFutures.push_back(MetaclusterAPI::deleteTenant(self->managementDb, t.second.second));
			}
			// We don't expect the data cluster tenant, so delete it
			else {
				removeTrackedTenant(t.second.first);
				deleteFutures.push_back(TenantAPI::deleteTenant(dataDb.getReference(), t.first, t.second.first));
			}
		}

		return waitForAll(deleteFutures);
	}

	ACTOR template <class Transaction>
	static Future<std::unordered_set<int64_t>> getTenantsInGroup(Transaction tr,
	                                                             TenantMetadataSpecification tenantMetadata,
	                                                             TenantGroupName tenantGroup) {
		KeyBackedRangeResult<Tuple> groupTenants =
		    wait(tenantMetadata.tenantGroupTenantIndex.getRange(tr,
		                                                        Tuple::makeTuple(tenantGroup),
		                                                        Tuple::makeTuple(keyAfter(tenantGroup)),
		                                                        CLIENT_KNOBS->MAX_TENANTS_PER_CLUSTER + 1));
		std::unordered_set<int64_t> tenants;
		for (auto const& tuple : groupTenants.results) {
			tenants.insert(tuple.getInt(1));
		}

		return tenants;
	}

	ACTOR Future<Void> resolveGroupCollisions(MetaclusterRestoreWorkload* self,
	                                          ClusterName clusterName,
	                                          Database dataDb,
	                                          GroupCollisions groupCollisions) {
		TraceEvent("MetaclusterRestoreWorkloadDeleteTenantGroupCollisions")
		    .detail("FromCluster", clusterName)
		    .detail("GroupCollisions", groupCollisions.size());

		state std::vector<Future<Void>> deleteFutures;

		state GroupCollisions::const_iterator collisionItr;
		for (collisionItr = groupCollisions.begin(); collisionItr != groupCollisions.end(); ++collisionItr) {
			// If the data cluster tenant group is expected, then remove the management tenant group
			// Note that the management tenant group may also have been expected
			auto itr = self->tenantGroups.find(*collisionItr);
			if (itr->second.cluster == clusterName) {
				TraceEvent(SevDebug, "MetaclusterRestoreWorkloadDeleteTenantGroupCollision")
				    .detail("From", "ManagementCluster")
				    .detail("TenantGroup", *collisionItr);
				std::unordered_set<int64_t> tenantsInGroup =
				    wait(runTransaction(self->managementDb, [collisionItr = collisionItr](Reference<ITransaction> tr) {
					    return getTenantsInGroup(
					        tr, MetaclusterAPI::ManagementClusterMetadata::tenantMetadata(), *collisionItr);
				    }));

				for (auto const& t : tenantsInGroup) {
					self->removeTrackedTenant(t);
					deleteFutures.push_back(MetaclusterAPI::deleteTenant(self->managementDb, t));
				}

			}
			// The tenant group from the management cluster is what we expect
			else {
				TraceEvent(SevDebug, "MetaclusterRestoreWorkloadDeleteTenantGroupCollision")
				    .detail("From", "DataCluster")
				    .detail("TenantGroup", *collisionItr);
				std::unordered_set<int64_t> tenantsInGroup = wait(runTransaction(
				    dataDb.getReference(), [collisionItr = collisionItr](Reference<ReadYourWritesTransaction> tr) {
					    tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
					    return getTenantsInGroup(tr, TenantMetadata::instance(), *collisionItr);
				    }));

				deleteFutures.push_back(runTransactionVoid(
				    dataDb.getReference(), [self = self, tenantsInGroup](Reference<ReadYourWritesTransaction> tr) {
					    tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
					    std::vector<Future<Void>> groupDeletions;
					    for (auto const& t : tenantsInGroup) {
						    self->removeTrackedTenant(t);
						    groupDeletions.push_back(TenantAPI::deleteTenantTransaction(tr, t));
					    }
					    return waitForAll(groupDeletions);
				    }));
			}
		}

		wait(waitForAll(deleteFutures));
		return Void();
	}

	ACTOR static Future<std::vector<std::pair<int64_t, TenantMapEntry>>> getDataClusterTenants(Database db) {
		KeyBackedRangeResult<std::pair<int64_t, TenantMapEntry>> tenants =
		    wait(runTransaction(db.getReference(), [](Reference<ReadYourWritesTransaction> tr) {
			    tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			    return TenantMetadata::tenantMap().getRange(tr, {}, {}, CLIENT_KNOBS->MAX_TENANTS_PER_CLUSTER + 1);
		    }));

		ASSERT_LE(tenants.results.size(), CLIENT_KNOBS->MAX_TENANTS_PER_CLUSTER);
		return tenants.results;
	}

	ACTOR static Future<std::pair<TenantCollisions, GroupCollisions>> getCollisions(MetaclusterRestoreWorkload* self,
	                                                                                Database db) {
		state KeyBackedRangeResult<std::pair<TenantName, int64_t>> managementTenantList;
		state KeyBackedRangeResult<std::pair<TenantGroupName, TenantGroupEntry>> managementGroupList;
		state KeyBackedRangeResult<std::pair<TenantName, int64_t>> dataClusterTenants;
		state KeyBackedRangeResult<std::pair<TenantGroupName, TenantGroupEntry>> dataClusterGroups;

		state TenantCollisions tenantCollisions;
		state GroupCollisions groupCollisions;

		// Read the management cluster tenant map and tenant group map
		wait(runTransactionVoid(
		    self->managementDb,
		    [managementTenantList = &managementTenantList,
		     managementGroupList = &managementGroupList](Reference<ITransaction> tr) {
			    return store(*managementTenantList,
			                 MetaclusterAPI::ManagementClusterMetadata::tenantMetadata().tenantNameIndex.getRange(
			                     tr, {}, {}, CLIENT_KNOBS->MAX_TENANTS_PER_CLUSTER + 1)) &&
			           store(*managementGroupList,
			                 MetaclusterAPI::ManagementClusterMetadata::tenantMetadata().tenantGroupMap.getRange(
			                     tr, {}, {}, CLIENT_KNOBS->MAX_TENANTS_PER_CLUSTER + 1));
		    }));

		// Read the data cluster tenant map and tenant group map
		wait(runTransaction(db.getReference(),
		                    [dataClusterTenants = &dataClusterTenants,
		                     dataClusterGroups = &dataClusterGroups](Reference<ReadYourWritesTransaction> tr) {
			                    tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			                    return store(*dataClusterTenants,
			                                 TenantMetadata::tenantNameIndex().getRange(
			                                     tr, {}, {}, CLIENT_KNOBS->MAX_TENANTS_PER_CLUSTER + 1)) &&
			                           store(*dataClusterGroups,
			                                 TenantMetadata::tenantGroupMap().getRange(
			                                     tr, {}, {}, CLIENT_KNOBS->MAX_TENANTS_PER_CLUSTER + 1));
		                    }));

		std::unordered_map<TenantName, int64_t> managementTenants(managementTenantList.results.begin(),
		                                                          managementTenantList.results.end());
		std::unordered_map<TenantGroupName, TenantGroupEntry> managementGroups(managementGroupList.results.begin(),
		                                                                       managementGroupList.results.end());

		ASSERT(managementTenants.size() <= CLIENT_KNOBS->MAX_TENANTS_PER_CLUSTER);
		ASSERT(managementGroups.size() <= CLIENT_KNOBS->MAX_TENANTS_PER_CLUSTER);
		ASSERT(dataClusterTenants.results.size() <= CLIENT_KNOBS->MAX_TENANTS_PER_CLUSTER);
		ASSERT(dataClusterGroups.results.size() <= CLIENT_KNOBS->MAX_TENANTS_PER_CLUSTER);

		for (auto const& t : dataClusterTenants.results) {
			auto itr = managementTenants.find(t.first);
			if (itr != managementTenants.end()) {
				tenantCollisions[t.first] = std::make_pair(t.second, itr->second);
			}
		}
		for (auto const& g : dataClusterGroups.results) {
			if (managementGroups.count(g.first)) {
				groupCollisions.insert(g.first);
			}
		}

		return std::make_pair(tenantCollisions, groupCollisions);
	}

	ACTOR static Future<Void> restoreManagementCluster(MetaclusterRestoreWorkload* self) {
		TraceEvent("MetaclusterRestoreWorkloadRestoringManagementCluster");
		wait(success(MetaclusterAPI::createMetacluster(
		    self->managementDb,
		    "management_cluster"_sr,
		    deterministicRandom()->randomInt(TenantAPI::TENANT_ID_PREFIX_MIN_VALUE,
		                                     TenantAPI::TENANT_ID_PREFIX_MAX_VALUE + 1))));
		state std::map<ClusterName, DataClusterData>::iterator clusterItr;
		for (clusterItr = self->dataDbs.begin(); clusterItr != self->dataDbs.end(); ++clusterItr) {
			TraceEvent("MetaclusterRestoreWorkloadProcessDataCluster").detail("FromCluster", clusterItr->first);

			// Remove the data cluster from its old metacluster
			wait(success(MetaclusterAPI::removeCluster(
			    clusterItr->second.db.getReference(), clusterItr->first, ClusterType::METACLUSTER_DATA, true)));
			TraceEvent("MetaclusterRestoreWorkloadForgotMetacluster").detail("ClusterName", clusterItr->first);

			state std::pair<TenantCollisions, GroupCollisions> collisions =
			    wait(getCollisions(self, clusterItr->second.db));

			state std::vector<std::string> messages;
			state bool completed = false;
			while (!completed) {
				state std::vector<std::pair<int64_t, TenantMapEntry>> dataTenantsBeforeRestore =
				    wait(getDataClusterTenants(clusterItr->second.db));

				try {
					TraceEvent("MetaclusterRestoreWorkloadRestoreManagementCluster")
					    .detail("FromCluster", clusterItr->first)
					    .detail("TenantCollisions", collisions.first.size());

					if (deterministicRandom()->coinflip()) {
						TraceEvent("MetaclusterRestoreWorkloadRestoreManagementClusterDryRun")
						    .detail("FromCluster", clusterItr->first)
						    .detail("TenantCollisions", collisions.first.size());

						wait(MetaclusterAPI::restoreCluster(
						    self->managementDb,
						    clusterItr->first,
						    clusterItr->second.db->getConnectionRecord()->getConnectionString(),
						    ApplyManagementClusterUpdates::False,
						    RestoreDryRun::True,
						    ForceJoinNewMetacluster(deterministicRandom()->coinflip()),
						    &messages));

						TraceEvent("MetaclusterRestoreWorkloadRestoreManagementClusterDryRunDone")
						    .detail("FromCluster", clusterItr->first)
						    .detail("TenantCollisions", collisions.first.size());

						messages.clear();
					}

					wait(MetaclusterAPI::restoreCluster(
					    self->managementDb,
					    clusterItr->first,
					    clusterItr->second.db->getConnectionRecord()->getConnectionString(),
					    ApplyManagementClusterUpdates::False,
					    RestoreDryRun::False,
					    ForceJoinNewMetacluster(deterministicRandom()->coinflip()),
					    &messages));

					ASSERT(collisions.first.empty() && collisions.second.empty());
					completed = true;
				} catch (Error& e) {
					bool failedDueToCollision =
					    (e.code() == error_code_tenant_already_exists && !collisions.first.empty()) ||
					    (e.code() == error_code_invalid_tenant_configuration && !collisions.second.empty());
					if (!failedDueToCollision) {
						throw;
					}

					// If the restore did not succeed, remove the partially restored cluster
					try {
						wait(success(MetaclusterAPI::removeCluster(
						    self->managementDb, clusterItr->first, ClusterType::METACLUSTER_MANAGEMENT, true)));
						TraceEvent("MetaclusterRestoreWorkloadRemoveFailedCluster")
						    .detail("ClusterName", clusterItr->first);
					} catch (Error& e) {
						if (e.code() != error_code_cluster_not_found) {
							throw;
						}
					}
				}

				std::vector<std::pair<int64_t, TenantMapEntry>> dataTenantsAfterRestore =
				    wait(getDataClusterTenants(clusterItr->second.db));

				// Restoring a management cluster from data clusters should not change the data clusters at all
				fmt::print("Checking data clusters: {}\n", completed);
				ASSERT_EQ(dataTenantsBeforeRestore.size(), dataTenantsAfterRestore.size());
				for (int i = 0; i < dataTenantsBeforeRestore.size(); ++i) {
					ASSERT_EQ(dataTenantsBeforeRestore[i].first, dataTenantsAfterRestore[i].first);
					ASSERT(dataTenantsBeforeRestore[i].second == dataTenantsAfterRestore[i].second);
				}

				// If we didn't succeed, resolve tenant and group collisions and try again
				if (!completed) {
					ASSERT(messages.size() > 0);

					wait(self->resolveTenantCollisions(
					    self, clusterItr->first, clusterItr->second.db, collisions.first));
					wait(self->resolveGroupCollisions(
					    self, clusterItr->first, clusterItr->second.db, collisions.second));

					collisions.first.clear();
					collisions.second.clear();
				}
			}
			TraceEvent("MetaclusterRestoreWorkloadRestoredDataClusterToManagementCluster")
			    .detail("FromCluster", clusterItr->first);
		}

		TraceEvent("MetaclusterRestoreWorkloadRestoredManagementCluster");
		return Void();
	}

	ACTOR static Future<Void> resetManagementCluster(MetaclusterRestoreWorkload* self) {
		state Reference<ITransaction> tr = self->managementDb->createTransaction();
		TraceEvent("MetaclusterRestoreWorkloadEraseManagementCluster");
		loop {
			try {
				tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
				tr->clear(""_sr, "\xff"_sr);
				MetaclusterMetadata::metaclusterRegistration().clear(tr);
				wait(safeThreadFutureToFuture(tr->commit()));
				TraceEvent("MetaclusterRestoreWorkloadManagementClusterErased");
				return Void();
			} catch (Error& e) {
				wait(safeThreadFutureToFuture(tr->onError(e)));
			}
		}
	}

	ACTOR static Future<Void> createTenant(MetaclusterRestoreWorkload* self, TenantData::AccessTime createTime) {
		state TenantName tenantName;
		for (int i = 0; i < 10; ++i) {
			tenantName = self->chooseTenantName();
			if (self->tenantNameIndex.count(tenantName) == 0) {
				break;
			}
		}

		if (self->tenantNameIndex.count(tenantName)) {
			return Void();
		}

		loop {
			try {
				TenantMapEntry tenantEntry;
				tenantEntry.tenantName = tenantName;
				tenantEntry.tenantGroup = self->chooseTenantGroup();
				wait(MetaclusterAPI::createTenant(self->managementDb, tenantEntry, AssignClusterAutomatically::True));
				TenantMapEntry createdEntry = wait(MetaclusterAPI::getTenant(self->managementDb, tenantName));
				TraceEvent(SevDebug, "MetaclusterRestoreWorkloadCreatedTenant")
				    .detail("Tenant", tenantName)
				    .detail("TenantId", createdEntry.id)
				    .detail("AccessTime", createTime);
				self->createdTenants[createdEntry.id] =
				    TenantData(tenantName, createdEntry.assignedCluster.get(), createdEntry.tenantGroup, createTime);
				self->tenantNameIndex[tenantName] = createdEntry.id;
				auto& dataDb = self->dataDbs[createdEntry.assignedCluster.get()];
				dataDb.tenants.insert(createdEntry.id);
				if (createdEntry.tenantGroup.present()) {
					auto& tenantGroupData = self->tenantGroups[createdEntry.tenantGroup.get()];
					tenantGroupData.cluster = createdEntry.assignedCluster.get();
					tenantGroupData.tenants.insert(createdEntry.id);
					dataDb.tenantGroups.insert(createdEntry.tenantGroup.get());
				}
				return Void();
			} catch (Error& e) {
				if (e.code() != error_code_metacluster_no_capacity) {
					throw;
				}

				wait(increaseMetaclusterCapacity(self));
			}
		}
	}

	ACTOR static Future<Void> deleteTenant(MetaclusterRestoreWorkload* self, TenantData::AccessTime accessTime) {
		state TenantName tenantName;
		for (int i = 0; i < 10; ++i) {
			tenantName = self->chooseTenantName();
			if (self->tenantNameIndex.count(tenantName) != 0) {
				break;
			}
		}

		if (self->tenantNameIndex.count(tenantName) == 0) {
			return Void();
		}

		state int64_t tenantId = self->tenantNameIndex[tenantName];

		TraceEvent(SevDebug, "MetaclusterRestoreWorkloadDeleteTenant")
		    .detail("Tenant", tenantName)
		    .detail("TenantId", tenantId)
		    .detail("AccessTime", accessTime);
		wait(MetaclusterAPI::deleteTenant(self->managementDb, tenantName));
		auto const& tenantData = self->createdTenants[tenantId];

		auto& dataDb = self->dataDbs[tenantData.cluster];
		dataDb.tenants.erase(tenantId);
		if (tenantData.tenantGroup.present()) {
			auto groupItr = self->tenantGroups.find(tenantData.tenantGroup.get());
			groupItr->second.tenants.erase(tenantId);
			if (groupItr->second.tenants.empty()) {
				self->tenantGroups.erase(groupItr);
				dataDb.tenantGroups.erase(tenantData.tenantGroup.get());
			}
		}

		self->createdTenants.erase(tenantId);
		self->tenantNameIndex.erase(tenantName);
		self->deletedTenants.insert(tenantId);

		return Void();
	}

	ACTOR static Future<Void> configureTenant(MetaclusterRestoreWorkload* self, TenantData::AccessTime accessTime) {
		state TenantName tenantName;
		for (int i = 0; i < 10; ++i) {
			tenantName = self->chooseTenantName();
			if (self->tenantNameIndex.count(tenantName) != 0) {
				break;
			}
		}

		if (self->tenantNameIndex.count(tenantName) == 0) {
			return Void();
		}

		state int64_t tenantId = self->tenantNameIndex[tenantName];

		state Optional<TenantGroupName> tenantGroup = self->chooseTenantGroup(self->createdTenants[tenantId].cluster);
		state std::map<Standalone<StringRef>, Optional<Value>> configurationParams = { { "tenant_group"_sr,
			                                                                             tenantGroup } };

		loop {
			try {
				wait(MetaclusterAPI::configureTenant(self->managementDb, tenantName, configurationParams));

				auto& tenantData = self->createdTenants[tenantId];

				TraceEvent(SevDebug, "MetaclusterRestoreWorkloadConfigureTenant")
				    .detail("Tenant", tenantName)
				    .detail("TenantId", tenantId)
				    .detail("OldTenantGroup", tenantData.tenantGroup)
				    .detail("NewTenantGroup", tenantGroup)
				    .detail("AccessTime", accessTime);

				if (tenantData.tenantGroup != tenantGroup) {
					auto& dataDb = self->dataDbs[tenantData.cluster];
					if (tenantData.tenantGroup.present()) {
						auto groupItr = self->tenantGroups.find(tenantData.tenantGroup.get());
						groupItr->second.tenants.erase(tenantId);
						if (groupItr->second.tenants.empty()) {
							self->tenantGroups.erase(groupItr);
							dataDb.tenantGroups.erase(tenantData.tenantGroup.get());
						}
					}

					if (tenantGroup.present()) {
						self->tenantGroups[tenantGroup.get()].tenants.insert(tenantId);
						dataDb.tenantGroups.insert(tenantGroup.get());
					}

					tenantData.tenantGroup = tenantGroup;
					tenantData.configureTime = accessTime;
				}
				return Void();
			} catch (Error& e) {
				if (e.code() != error_code_cluster_no_capacity) {
					throw;
				}

				wait(increaseMetaclusterCapacity(self));
			}
		}
	}

	ACTOR static Future<Void> renameTenant(MetaclusterRestoreWorkload* self, TenantData::AccessTime accessTime) {
		state TenantName oldTenantName;
		state TenantName newTenantName;
		for (int i = 0; i < 10; ++i) {
			oldTenantName = self->chooseTenantName();
			if (self->tenantNameIndex.count(oldTenantName) != 0) {
				break;
			}
		}
		for (int i = 0; i < 10; ++i) {
			newTenantName = self->chooseTenantName();
			if (self->tenantNameIndex.count(newTenantName) == 0) {
				break;
			}
		}

		if (self->tenantNameIndex.count(oldTenantName) == 0 || self->tenantNameIndex.count(newTenantName) != 0) {
			return Void();
		}

		state int64_t tenantId = self->tenantNameIndex[oldTenantName];

		TraceEvent(SevDebug, "MetaclusterRestoreWorkloadRenameTenant")
		    .detail("OldTenantName", oldTenantName)
		    .detail("NewTenantName", newTenantName)
		    .detail("TenantId", tenantId)
		    .detail("AccessTime", accessTime);
		wait(MetaclusterAPI::renameTenant(self->managementDb, oldTenantName, newTenantName));

		TenantData& tenantData = self->createdTenants[tenantId];
		tenantData.name = newTenantName;
		tenantData.renameTime = accessTime;
		self->tenantNameIndex[newTenantName] = tenantId;
		self->tenantNameIndex.erase(oldTenantName);

		return Void();
	}

	ACTOR static Future<Void> runOperations(MetaclusterRestoreWorkload* self) {
		while (now() < self->endTime) {
			state int operation = deterministicRandom()->randomInt(0, 4);
			state TenantData::AccessTime accessTime =
			    self->backupComplete ? TenantData::AccessTime::AFTER_BACKUP : TenantData::AccessTime::DURING_BACKUP;
			if (operation == 0) {
				wait(createTenant(self, accessTime));
			} else if (operation == 1) {
				wait(deleteTenant(self, accessTime));
			} else if (operation == 2) {
				wait(configureTenant(self, accessTime));
			} else if (operation == 3) {
				wait(renameTenant(self, accessTime));
			}
		}

		return Void();
	}

	Future<Void> start(Database const& cx) override {
		if (clientId == 0) {
			return _start(cx, this);
		} else {
			return Void();
		}
	}
	ACTOR static Future<Void> _start(Database cx, MetaclusterRestoreWorkload* self) {
		state std::set<ClusterName> clustersToRestore;

		TraceEvent("MetaclusterRestoreWorkloadStart")
		    .detail("RecoverManagementCluster", self->recoverManagementCluster)
		    .detail("RecoverDataClusters", self->recoverDataClusters);

		if (self->recoverDataClusters) {
			for (auto db : self->dataDbIndex) {
				if (deterministicRandom()->random01() < 0.1) {
					clustersToRestore.insert(db);
				}
			}

			if (clustersToRestore.empty()) {
				clustersToRestore.insert(deterministicRandom()->randomChoice(self->dataDbIndex));
			}

			for (auto c : clustersToRestore) {
				TraceEvent("MetaclusterRestoreWorkloadChoseClusterForRestore").detail("ClusterName", c);
			}
		}

		state Future<Void> opsFuture = runOperations(self);

		state std::map<ClusterName, Future<std::string>> backups;
		for (auto cluster : clustersToRestore) {
			backups[cluster] = backupCluster(cluster, self->dataDbs[cluster].db, self);
		}

		for (auto [_, f] : backups) {
			wait(success(f));
		}

		self->backupComplete = true;
		self->endTime = now() + 30.0;

		wait(opsFuture);
		TraceEvent("MetaclusterRestoreWorkloadOperationsComplete");

		if (self->recoverManagementCluster) {
			wait(resetManagementCluster(self));
		} else {
			KeyBackedRangeResult<std::pair<int64_t, TenantMapEntry>> tenants =
			    wait(runTransaction(self->managementDb, [](Reference<ITransaction> tr) {
				    return MetaclusterAPI::ManagementClusterMetadata::tenantMetadata().tenantMap.getRange(
				        tr, {}, {}, CLIENT_KNOBS->MAX_TENANTS_PER_CLUSTER + 1);
			    }));
			ASSERT_LE(tenants.results.size(), CLIENT_KNOBS->MAX_TENANTS_PER_CLUSTER);
			self->managementTenantsBeforeRestore = tenants.results;
		}

		std::vector<Future<Void>> restores;
		for (auto [cluster, backupUrl] : backups) {
			restores.push_back(restoreDataCluster(cluster,
			                                      self->dataDbs[cluster].db,
			                                      backupUrl.get(),
			                                      !self->recoverManagementCluster,
			                                      ForceJoinNewMetacluster(deterministicRandom()->coinflip()),
			                                      self));
		}

		wait(waitForAll(restores));

		if (self->recoverManagementCluster) {
			wait(restoreManagementCluster(self));

			if (deterministicRandom()->coinflip()) {
				std::vector<Future<Void>> secondRestores;
				for (auto [cluster, backupUrl] : backups) {
					secondRestores.push_back(restoreDataCluster(cluster,
					                                            self->dataDbs[cluster].db,
					                                            backupUrl.get(),
					                                            true,
					                                            ForceJoinNewMetacluster::True,
					                                            self));
				}
				wait(waitForAll(secondRestores));
			}
		}

		return Void();
	}

	// Checks that the data cluster state matches our local state
	ACTOR static Future<Void> checkDataCluster(MetaclusterRestoreWorkload* self,
	                                           ClusterName clusterName,
	                                           DataClusterData clusterData) {
		state Optional<MetaclusterRegistrationEntry> metaclusterRegistration;
		state KeyBackedRangeResult<std::pair<int64_t, TenantMapEntry>> tenants;
		state Reference<ReadYourWritesTransaction> tr = clusterData.db->createTransaction();

		loop {
			try {
				tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
				wait(
				    store(metaclusterRegistration, MetaclusterMetadata::metaclusterRegistration().get(tr)) &&
				    store(tenants,
				          TenantMetadata::tenantMap().getRange(tr, {}, {}, CLIENT_KNOBS->MAX_TENANTS_PER_CLUSTER + 1)));
				break;
			} catch (Error& e) {
				wait(safeThreadFutureToFuture(tr->onError(e)));
			}
		}
		ASSERT_LE(tenants.results.size(), CLIENT_KNOBS->MAX_TENANTS_PER_CLUSTER);

		ASSERT(metaclusterRegistration.present() &&
		       metaclusterRegistration.get().clusterType == ClusterType::METACLUSTER_DATA);

		if (!clusterData.restored) {
			ASSERT_EQ(tenants.results.size(), clusterData.tenants.size());
			for (auto [tenantId, tenantEntry] : tenants.results) {
				ASSERT(clusterData.tenants.count(tenantId));
				auto tenantData = self->createdTenants[tenantId];
				ASSERT(tenantData.cluster == clusterName);
				ASSERT(tenantData.tenantGroup == tenantEntry.tenantGroup);
				ASSERT(tenantData.name == tenantEntry.tenantName);
			}
		} else {
			int expectedTenantCount = 0;
			std::map<int64_t, TenantMapEntry> tenantMap(tenants.results.begin(), tenants.results.end());
			for (auto tenantId : clusterData.tenants) {
				TenantData tenantData = self->createdTenants[tenantId];
				auto tenantItr = tenantMap.find(tenantId);
				if (tenantData.createTime == TenantData::AccessTime::BEFORE_BACKUP) {
					++expectedTenantCount;
					ASSERT(tenantItr != tenantMap.end());
					ASSERT(tenantData.cluster == clusterName);
					if (!self->recoverManagementCluster ||
					    tenantData.configureTime <= TenantData::AccessTime::BEFORE_BACKUP) {
						ASSERT(tenantItr->second.tenantGroup == tenantData.tenantGroup);
					}
					if (!self->recoverManagementCluster ||
					    tenantData.renameTime <= TenantData::AccessTime::BEFORE_BACKUP) {
						ASSERT(tenantItr->second.tenantName == tenantData.name);
					}
				} else if (tenantData.createTime == TenantData::AccessTime::AFTER_BACKUP) {
					ASSERT(tenantItr == tenantMap.end());
				} else if (tenantItr != tenantMap.end()) {
					++expectedTenantCount;
				}
			}

			// Check for deleted tenants that reappeared
			int unexpectedTenants = 0;
			for (auto const& [tenantId, tenantEntry] : tenantMap) {
				if (!clusterData.tenants.count(tenantId)) {
					ASSERT(self->recoverManagementCluster);
					ASSERT(self->deletedTenants.count(tenantId));
					++unexpectedTenants;
				}
			}

			ASSERT_EQ(tenantMap.size() - unexpectedTenants, expectedTenantCount);
		}

		return Void();
	}

	ACTOR static Future<Void> checkTenants(MetaclusterRestoreWorkload* self) {
		state KeyBackedRangeResult<std::pair<int64_t, TenantMapEntry>> tenants =
		    wait(runTransaction(self->managementDb, [](Reference<ITransaction> tr) {
			    return MetaclusterAPI::ManagementClusterMetadata::tenantMetadata().tenantMap.getRange(
			        tr, {}, {}, CLIENT_KNOBS->MAX_TENANTS_PER_CLUSTER + 1);
		    }));

		ASSERT_LE(tenants.results.size(), CLIENT_KNOBS->MAX_TENANTS_PER_CLUSTER);
		std::map<int64_t, TenantMapEntry> tenantMap(tenants.results.begin(), tenants.results.end());

		// If we did not restore the management cluster, then every tenant present in the management cluster before the
		// restore should be present after the restore. All tenants in the management cluster should be unchanged except
		// for those tenants that were created after the backup and lost during the restore, which will be marked in an
		// error state.
		for (auto const& [tenantId, tenantEntry] : self->managementTenantsBeforeRestore) {
			auto itr = tenantMap.find(tenantId);
			ASSERT(itr != tenantMap.end());

			TenantMapEntry postRecoveryEntry = itr->second;
			if (postRecoveryEntry.tenantState == TenantState::ERROR) {
				ASSERT(self->dataDbs[itr->second.assignedCluster.get()].restored);
				postRecoveryEntry.tenantState = tenantEntry.tenantState;
				postRecoveryEntry.error.clear();
			}

			ASSERT(tenantEntry == postRecoveryEntry);
		}

		if (!self->managementTenantsBeforeRestore.empty()) {
			ASSERT_EQ(self->managementTenantsBeforeRestore.size(), tenantMap.size());
		}

		for (auto const& [tenantId, tenantData] : self->createdTenants) {
			auto tenantItr = tenantMap.find(tenantId);
			if (tenantItr == tenantMap.end()) {
				// A tenant that we expected to have been created can only be missing from the management cluster if we
				// lost data in the process of recovering both the management and some data clusters
				ASSERT_NE(tenantData.createTime, TenantData::AccessTime::BEFORE_BACKUP);
				ASSERT(self->dataDbs[tenantData.cluster].restored && self->recoverManagementCluster);
			} else {
				if (tenantData.createTime != TenantData::AccessTime::BEFORE_BACKUP &&
				    self->dataDbs[tenantData.cluster].restored) {
					ASSERT(tenantItr->second.tenantState == TenantState::ERROR ||
					       (tenantItr->second.tenantState == TenantState::READY &&
					        tenantData.createTime == TenantData::AccessTime::DURING_BACKUP));
					if (tenantItr->second.tenantState == TenantState::ERROR) {
						ASSERT(self->dataDbs[tenantData.cluster].restoreHasMessages);
					}
				} else {
					ASSERT_EQ(tenantItr->second.tenantState, TenantState::READY);
				}
			}
		}

		// If we recovered both the management and some data clusters, we might undelete a tenant
		// Check that any unexpected tenants were deleted and that we had a potentially lossy recovery
		for (auto const& [tenantId, tenantEntry] : tenantMap) {
			if (!self->createdTenants.count(tenantId)) {
				ASSERT(self->deletedTenants.count(tenantId));
				ASSERT(self->recoverManagementCluster);
				ASSERT(self->recoverDataClusters);
			}
		}

		return Void();
	}

	Future<bool> check(Database const& cx) override {
		if (clientId == 0) {
			return _check(this);
		} else {
			return true;
		}
	}
	ACTOR static Future<bool> _check(MetaclusterRestoreWorkload* self) {
		// The metacluster consistency check runs the tenant consistency check for each cluster
		state MetaclusterConsistencyCheck<IDatabase> metaclusterConsistencyCheck(
		    self->managementDb, AllowPartialMetaclusterOperations::True);

		wait(metaclusterConsistencyCheck.run());

		std::vector<Future<Void>> dataClusterChecks;
		for (auto [clusterName, dataClusterData] : self->dataDbs) {
			dataClusterChecks.push_back(checkDataCluster(self, clusterName, dataClusterData));
		}
		wait(waitForAll(dataClusterChecks));
		wait(checkTenants(self));
		return true;
	}

	void getMetrics(std::vector<PerfMetric>& m) override {}
};

WorkloadFactory<MetaclusterRestoreWorkload> MetaclusterRestoreWorkloadFactory;
