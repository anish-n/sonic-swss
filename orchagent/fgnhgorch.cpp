#include <assert.h>
#include <inttypes.h>
#include "fgnhgorch.h"
#include "routeorch.h"
#include "logger.h"
#include "swssnet.h"
#include "crmorch.h"
#include <array>
#include <algorithm>

#define LINK_DOWN    0
#define LINK_UP      1

extern sai_object_id_t gVirtualRouterId;
extern sai_object_id_t gSwitchId;

extern sai_next_hop_group_api_t*    sai_next_hop_group_api;
extern sai_route_api_t*             sai_route_api;

extern RouteOrch *gRouteOrch;
extern CrmOrch *gCrmOrch;
extern PortsOrch *gPortsOrch;

FgNhgOrch::FgNhgOrch(DBConnector *db, DBConnector *stateDb, vector<table_name_with_pri_t> &tableNames, NeighOrch *neighOrch, IntfsOrch *intfsOrch, VRFOrch *vrfOrch) :
        Orch(db, tableNames),
        m_neighOrch(neighOrch),
        m_intfsOrch(intfsOrch),
        m_vrfOrch(vrfOrch),
		m_stateWarmRestartRouteTable(stateDb, STATE_FG_ROUTE_TABLE_NAME)
{
    SWSS_LOG_ENTER();
    isFineGrainedConfigured = false;
    gPortsOrch->attach(this);
}


void FgNhgOrch::update(SubjectType type, void *cntx)
{
    SWSS_LOG_ENTER();
    assert(cntx);

    switch(type) {
        case SUBJECT_TYPE_PORT_OPER_STATE_CHANGE:
        {
            PortOperStateUpdate *update = reinterpret_cast<PortOperStateUpdate *>(cntx);
            for (auto &fgNhgEntry : m_FgNhgs)
            {
                auto entry = fgNhgEntry.second.links.find(update->port.m_alias);
                if (entry != fgNhgEntry.second.links.end())
                {
                    for (auto ip : entry->second)
                    {
                        NextHopKey nhk;
                        MacAddress macAddress;
                        auto nexthop_entry = fgNhgEntry.second.next_hops.find(ip);

                        if (update->operStatus == SAI_PORT_OPER_STATUS_UP)
                        {
                            if (nexthop_entry == fgNhgEntry.second.next_hops.end())
                            {
                                SWSS_LOG_WARN("Hit unexpected condition where structs are out of sync");
                            }
                            nexthop_entry->second.link_oper_state = LINK_UP;
                            SWSS_LOG_INFO("Updated %s assoicated with %s to state up",
                                    update->port.m_alias.c_str(), ip.to_string().c_str());

                            if (!m_neighOrch->getNeighborEntry(ip, nhk, macAddress))
                            {
                                continue;
                            }
 
                            if (!validNextHopInNextHopGroup(nhk))
                            {
                                SWSS_LOG_WARN("Failed validNextHopInNextHopGroup for nh %s ip %s",
                                        nhk.to_string().c_str(), ip.to_string().c_str());
                            }
                        }
                        else if (update->operStatus == SAI_PORT_OPER_STATUS_DOWN)
                        {
                            if (nexthop_entry == fgNhgEntry.second.next_hops.end())
                            {
                                SWSS_LOG_WARN("Hit unexpected condition where structs are out of sync");
                            }
                            nexthop_entry->second.link_oper_state = LINK_DOWN;
                            SWSS_LOG_INFO("Updated %s associated with %s to state down",
                                    update->port.m_alias.c_str(), ip.to_string().c_str());

                            if (!m_neighOrch->getNeighborEntry(ip, nhk, macAddress))
                            {
                                continue;
                            }

                            if (!invalidNextHopInNextHopGroup(nhk))
                            {
                                SWSS_LOG_WARN("Failed validNextHopInNextHopGroup for nh %s ip %s",
                                        nhk.to_string().c_str(), ip.to_string().c_str());
                            }
                        }
                    }
                }
            }
            break;
        }
        default:
            break;
    }
}

bool FgNhgOrch::bake()
{
    SWSS_LOG_ENTER();

    deque<KeyOpFieldsValuesTuple> entries;
    vector<string> keys;
    m_stateWarmRestartRouteTable.getKeys(keys);

    SWSS_LOG_NOTICE("Warm reboot: recovering entry %lu from state", keys.size());

    for (const auto &key : keys)
    {
        vector<FieldValueTuple> tuples;
        m_stateWarmRestartRouteTable.get(key, tuples);

        NextHopIndexMap nhop_index_map(tuples.size(), std::string());
        for (const auto &tuple : tuples)
        {
            const auto index = stoi(fvField(tuple));
            const auto nextHop = fvValue(tuple);
            SWSS_LOG_INFO("Found next hop %s with index %d before warm reboot",
                    nextHop.c_str(), index);

            nhop_index_map[index] = nextHop;
            SWSS_LOG_INFO("Storing next hop %s", nhop_index_map[index].c_str());
        }

        // Recover nexthop with index relationship
        m_recoveryMap[key] = nhop_index_map;

        remove_state_db_route_entry(key);
    }

    return Orch::bake();
}


/* calculateBankHashBucketStartIndices: generates the hash_bucket_indices for all banks
 * and stores it in fgNhgEntry for the group. 
 * The function will identify the # of next-hops assigned to each bank and 
 * assign the total number of hash buckets for a bank, based on the proportional
 * number of next-hops in the bank. 
 * eg: Bank0: 6 nh, Bank1: 3 nh, total buckets: 30 => 
 *      calculateBankHashBucketStartIndices: Bank0: Bucket# 0-19, Bank1: Bucket# 20-29
 */
void calculate_bank_hash_bucket_start_indices(FgNhgEntry *fgNhgEntry)
{
    SWSS_LOG_ENTER();
    uint32_t num_banks = 0;
    vector<uint32_t> memb_per_bank;
    for (auto nh : fgNhgEntry->next_hops)
    {
        while (nh.second.bank + 1 > num_banks)
        {
            num_banks++;
            memb_per_bank.push_back(0);
        }
        memb_per_bank[nh.second.bank] = memb_per_bank[nh.second.bank] + 1;
    }

    uint32_t buckets_per_nexthop = fgNhgEntry->real_bucket_size/((uint32_t)fgNhgEntry->next_hops.size());
    uint32_t extra_buckets = fgNhgEntry->real_bucket_size - (buckets_per_nexthop*((uint32_t)fgNhgEntry->next_hops.size()));
    uint32_t split_extra_buckets_among_bank = extra_buckets/num_banks;
    extra_buckets = extra_buckets - (split_extra_buckets_among_bank*num_banks);

    uint32_t prev_idx = 0;

    for(uint32_t i = 0; i < memb_per_bank.size(); i++)
    {
        bank_index_range bir;
        bir.start_index = prev_idx;
        bir.end_index = bir.start_index + (buckets_per_nexthop * memb_per_bank[i]) + split_extra_buckets_among_bank - 1;
        if(extra_buckets > 0)
        {
            bir.end_index = bir.end_index + 1;
            extra_buckets--;
        }
        if(i == fgNhgEntry->hash_bucket_indices.size())
        {
            fgNhgEntry->hash_bucket_indices.push_back(bir);
        }
        else
        {
            fgNhgEntry->hash_bucket_indices[i] = bir;
        }
        prev_idx = bir.end_index + 1;
        SWSS_LOG_INFO("Calculate_bank_hash_bucket_start_indices: bank %d, si %d, ei %d",
                       i, fgNhgEntry->hash_bucket_indices[i].start_index, fgNhgEntry->hash_bucket_indices[i].end_index);
    }
}

void FgNhgOrch::remove_state_db_route_entry(const string& ipPrefix)
{
	SWSS_LOG_ENTER();

	m_stateWarmRestartRouteTable.del(ipPrefix);
}

void FgNhgOrch::set_state_db_route_entry(const IpPrefix &ipPrefix, uint32_t index, NextHopKey nextHop)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_INFO("Enter set state db entry for ip prefix %s next hop %s with index %d",
                                    ipPrefix.to_string().c_str(), nextHop.to_string().c_str(), index);
    string key = ipPrefix.to_string();
    // Write to StateDb
    std::vector<FieldValueTuple> fvs;

    // check if profile already exists - if yes - skip creation
    m_stateWarmRestartRouteTable.get(key, fvs);

    //bucket rewrite
    if (fvs.size() > index)
    {
        FieldValueTuple fv(std::to_string(index), nextHop.to_string());
        fvs[index] = fv;
        SWSS_LOG_INFO("Set state db entry for ip prefix %s next hop %s with index %d",
                        ipPrefix.to_string().c_str(), nextHop.to_string().c_str(), index);
        m_stateWarmRestartRouteTable.set(key, fvs);

    }
    else
    {
        fvs.push_back(FieldValueTuple(std::to_string(index), nextHop.to_string()));
        SWSS_LOG_INFO("Add new next hop entry %s with index %d for ip prefix %s",
                nextHop.to_string().c_str(), index, ipPrefix.to_string().c_str());
        m_stateWarmRestartRouteTable.set(key, fvs);
    }

}


bool FgNhgOrch::write_hash_bucket_change_to_sai(FGNextHopGroupEntry *syncd_fg_route_entry, uint32_t index, sai_object_id_t nh_oid,
        const IpPrefix &ipPrefix, NextHopKey nextHop)
{
    SWSS_LOG_ENTER();

    sai_attribute_t nhgm_attr;
    nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
    nhgm_attr.value.oid = nh_oid;
    sai_status_t status = sai_next_hop_group_api->set_next_hop_group_member_attribute(
                                                              syncd_fg_route_entry->nhopgroup_members[index],
                                                              &nhgm_attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set next hop oid %" PRIx64 " member %" PRIx64 ": %d\n",
            syncd_fg_route_entry->nhopgroup_members[index], nh_oid, status);
        return false;
    }

    set_state_db_route_entry(ipPrefix, index, nextHop);
    return true;
}


bool FgNhgOrch::modifyRoutesNextHopId(sai_object_id_t vrf_id, const IpPrefix &ipPrefix, sai_object_id_t next_hop_id)
{
    sai_route_entry_t route_entry;
    sai_attribute_t route_attr;

    route_entry.vr_id = vrf_id;
    route_entry.switch_id = gSwitchId;
    copy(route_entry.destination, ipPrefix);

    route_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    route_attr.value.oid = next_hop_id;

    sai_status_t status = sai_route_api->set_route_entry_attribute(&route_entry, &route_attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set route %s with packet action forward, %d",
                       ipPrefix.to_string().c_str(), status);
        return false;
    }

    return true;
}


bool FgNhgOrch::validNextHopInNextHopGroup(const NextHopKey& nexthop)
{
    SWSS_LOG_ENTER();

    for( auto &route_tables : m_syncdFGRouteTables )
    {
        for( auto &route_table : route_tables.second )
        {
            if(!(route_table.second.nhg_key.contains(nexthop)))
            {
                continue;
            }

            FGNextHopGroupEntry *syncd_fg_route_entry = &(route_table.second);
            FgNhgEntry *fgNhgEntry = 0;
            auto prefix_entry = m_fgNhgPrefixes.find(route_table.first);
            if (prefix_entry == m_fgNhgPrefixes.end())
            {
                auto member_entry = m_fgNhgNexthops.find(nexthop.ip_address);
                if (member_entry == m_fgNhgNexthops.end())
                {
                    SWSS_LOG_ERROR("fgNhgOrch got a validNextHopInNextHopGroup for non-configured FG ECMP entry");
                    return false;
                }
                fgNhgEntry = member_entry->second;
            }
            else 
            {
                fgNhgEntry = prefix_entry->second;
            }
            std::map<NextHopKey,sai_object_id_t> nhopgroup_members_set;

            std::vector<Bank_Member_Changes> bank_member_changes(
                fgNhgEntry->hash_bucket_indices.size(), Bank_Member_Changes());

            if(syncd_fg_route_entry->active_nexthops.find(nexthop) !=
                    syncd_fg_route_entry->active_nexthops.end())
            {
                return true;
            }


            if(fgNhgEntry->hash_bucket_indices.size() == 0 && syncd_fg_route_entry->points_to_rif)
            {
                /* Only happens the 1st time when hash_bucket_indices are not inited
                 */
                for (auto it : fgNhgEntry->next_hops)
                {
                    while(bank_member_changes.size() <= it.second.bank)
                    {
                        bank_member_changes.push_back(Bank_Member_Changes());
                    }
                }
            }

            bank_member_changes[fgNhgEntry->next_hops[nexthop.ip_address].bank].
                    nhs_to_add.push_back(nexthop);
            nhopgroup_members_set[nexthop] = m_neighOrch->getNextHopId(nexthop);

            if (syncd_fg_route_entry->points_to_rif)
            {
                // RIF route is now neigh resolved: create Fine Grained ECMP
                if (!createFgNhg(route_tables.first, route_table.first, *syncd_fg_route_entry, fgNhgEntry, 
                                bank_member_changes, nhopgroup_members_set))
                {
                    return false;
                }

                if (!modifyRoutesNextHopId(route_tables.first, route_table.first, syncd_fg_route_entry->next_hop_group_id))
                {
                    return false;
                }
            }
            else
            {
                for( auto active_nh : syncd_fg_route_entry->active_nexthops)
                {
                    bank_member_changes[fgNhgEntry->next_hops[active_nh.ip_address].bank].
                        active_nhs.push_back(active_nh);
                }

                if(!compute_and_set_hash_bucket_changes(syncd_fg_route_entry, fgNhgEntry, 
                        bank_member_changes, nhopgroup_members_set, route_table.first))
                {
                    SWSS_LOG_ERROR("Failed to set fine grained next hop %s",
                        nexthop.to_string().c_str());
                    return false;
                }
            }

            m_neighOrch->increaseNextHopRefCount(nexthop);
            SWSS_LOG_INFO("FG nh %s for prefix %s is up",
                    nexthop.to_string().c_str(), route_table.first.to_string().c_str());
        }
    }

    return true;
}


bool FgNhgOrch::invalidNextHopInNextHopGroup(const NextHopKey& nexthop)
{
    SWSS_LOG_ENTER();

    for( auto &route_tables : m_syncdFGRouteTables )
    {
        for( auto &route_table : route_tables.second )
        {
            if(!(route_table.second.nhg_key.contains(nexthop)))
            {
                continue;
            }

            FGNextHopGroupEntry *syncd_fg_route_entry = &(route_table.second);
            FgNhgEntry *fgNhgEntry = 0;
            auto prefix_entry = m_fgNhgPrefixes.find(route_table.first);
            if (prefix_entry == m_fgNhgPrefixes.end())
            {
                auto member_entry = m_fgNhgNexthops.find(nexthop.ip_address);
                if (member_entry == m_fgNhgNexthops.end())
                {
                    SWSS_LOG_ERROR("fgNhgOrch got an invalidNextHopInNextHopGroup for non-configured FG ECMP entry");
                    return false;
                }
                fgNhgEntry = member_entry->second;
            }
            else 
            {
                fgNhgEntry = prefix_entry->second;
            }

            std::map<NextHopKey,sai_object_id_t> nhopgroup_members_set;

            std::vector<Bank_Member_Changes> bank_member_changes(
                fgNhgEntry->hash_bucket_indices.size(), Bank_Member_Changes());

            if(syncd_fg_route_entry->active_nexthops.find(nexthop) ==
                    syncd_fg_route_entry->active_nexthops.end())
            {
                return true;
            }

            for( auto active_nh : syncd_fg_route_entry->active_nexthops)
            {
                if(active_nh.ip_address == nexthop.ip_address &&
                        active_nh.alias == nexthop.alias)
                {
                    continue;
                }

                bank_member_changes[fgNhgEntry->next_hops[active_nh.ip_address].bank].
                    active_nhs.push_back(active_nh);

                nhopgroup_members_set[active_nh] = m_neighOrch->getNextHopId(active_nh);
            }

            bank_member_changes[fgNhgEntry->next_hops[nexthop.ip_address].bank].
                    nhs_to_del.push_back(nexthop);

            if(!compute_and_set_hash_bucket_changes(syncd_fg_route_entry, fgNhgEntry, 
                    bank_member_changes, nhopgroup_members_set, route_table.first))
            {
                SWSS_LOG_ERROR("Failed to set fine grained next hop %s",
                    nexthop.to_string().c_str());
                return false;
            }

            m_neighOrch->decreaseNextHopRefCount(nexthop);
            SWSS_LOG_INFO("FG nh %s for prefix %s is down",
                    nexthop.to_string().c_str(), route_table.first.to_string().c_str());
        }
    }

    return true;
}


bool FgNhgOrch::remove_nhg(FGNextHopGroupEntry *syncd_fg_route_entry)
{
    SWSS_LOG_ENTER();
    sai_status_t status;

    for(auto nhgm : syncd_fg_route_entry->nhopgroup_members)
    {
        status = sai_next_hop_group_api->remove_next_hop_group_member(nhgm);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove next hop group member %" PRIx64 ", rv:%d",
                nhgm, status);
            return false;
        }
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
    }

    status = sai_next_hop_group_api->remove_next_hop_group(syncd_fg_route_entry->next_hop_group_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove next hop group %" PRIx64 ", rv:%d",
                syncd_fg_route_entry->next_hop_group_id, status);
        return false;
    }
    gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);
    gRouteOrch->decNextHopGroupCount();

    return true;
}


bool FgNhgOrch::set_active_bank_hash_bucket_changes(FGNextHopGroupEntry *syncd_fg_route_entry, FgNhgEntry *fgNhgEntry,
        uint32_t bank, uint32_t syncd_bank, std::vector<Bank_Member_Changes> bank_member_changes, 
        std::map<NextHopKey,sai_object_id_t> &nhopgroup_members_set, const IpPrefix &ipPrefix)
{
    SWSS_LOG_ENTER();

    Bank_Member_Changes bank_member_change = bank_member_changes[bank];

    uint32_t add_idx = 0, del_idx = 0;
    FGNextHopGroupMap *bank_fgnhg_map = &(syncd_fg_route_entry->syncd_fgnhg_map[syncd_bank]);

    while(del_idx < bank_member_change.nhs_to_del.size() &&
            add_idx < bank_member_change.nhs_to_add.size())
    {
        HashBuckets *hash_buckets = &(bank_fgnhg_map->at(bank_member_change.nhs_to_del[del_idx]));
        for(uint32_t i = 0; i < hash_buckets->size(); i++)
        {
            if(!write_hash_bucket_change_to_sai(syncd_fg_route_entry, hash_buckets->at(i), 
                        nhopgroup_members_set[bank_member_change.nhs_to_add[add_idx]],
                        ipPrefix, bank_member_change.nhs_to_add[add_idx]))
            {
                return false;
            }
        }

        (*bank_fgnhg_map)[bank_member_change.nhs_to_add[add_idx]] =*hash_buckets;

        bank_fgnhg_map->erase(bank_member_change.nhs_to_del[del_idx]);
        bank_member_change.active_nhs.push_back(bank_member_change.nhs_to_add[add_idx]);
        syncd_fg_route_entry->active_nexthops.erase(bank_member_change.nhs_to_del[del_idx]);
        syncd_fg_route_entry->active_nexthops.insert(bank_member_change.nhs_to_add[add_idx]);

        del_idx++;
        add_idx++;
    }

    /* Given that we resolved add + del on a bank in the above while stmt
     * We will either have add OR delete left to do, and the logic below 
     * relies on this fact
     */
    if(del_idx < bank_member_change.nhs_to_del.size())
    {
        uint32_t num_buckets_in_bank = 1 + fgNhgEntry->hash_bucket_indices[syncd_bank].end_index -
            fgNhgEntry->hash_bucket_indices[syncd_bank].start_index;
        uint32_t exp_bucket_size = num_buckets_in_bank / (uint32_t)bank_member_change.active_nhs.size();
        uint32_t num_nhs_with_one_more = (num_buckets_in_bank % (uint32_t)bank_member_change.active_nhs.size());


        while(del_idx < bank_member_change.nhs_to_del.size())
        {
            HashBuckets *hash_buckets = &(bank_fgnhg_map->at(bank_member_change.nhs_to_del[del_idx]));
            for(uint32_t i = 0; i < hash_buckets->size(); i++)
            {
                NextHopKey round_robin_nh = bank_member_change.active_nhs[i %
                    bank_member_change.active_nhs.size()];

                if(!write_hash_bucket_change_to_sai(syncd_fg_route_entry, hash_buckets->at(i), 
                        nhopgroup_members_set[round_robin_nh], ipPrefix, round_robin_nh))
                {
                    return false;
                }
                bank_fgnhg_map->at(round_robin_nh).push_back(hash_buckets->at(i));

                /* Logic below ensure that # hash buckets assigned to a nh is equalized */
                if(num_nhs_with_one_more == 0)
                {
                    if(bank_fgnhg_map->at(round_robin_nh).size() == exp_bucket_size)
                    {
                        SWSS_LOG_INFO("%s reached %d, don't remove more buckets", 
                                (bank_member_change.active_nhs[i % bank_member_change.active_nhs.size()]).to_string().c_str(), 
                                exp_bucket_size);
                        bank_member_change.active_nhs.erase(bank_member_change.active_nhs.begin() + 
                            (i % bank_member_change.active_nhs.size()));
                    }
                    else if(bank_fgnhg_map->at(round_robin_nh).size() > exp_bucket_size)
                    {
                        SWSS_LOG_WARN("Unexpected bucket size for nh %s, size %lu, exp_size %d",
                                round_robin_nh.to_string().c_str(), bank_fgnhg_map->at(round_robin_nh).size(),
                                exp_bucket_size);
                    }
                }
                else
                {
                    if(bank_fgnhg_map->at(round_robin_nh).size() == exp_bucket_size +1)
                    {

                        SWSS_LOG_INFO("%s reached %d, don't remove more buckets num_nhs_with_one_more %d", 
                                (bank_member_change.active_nhs[i %bank_member_change.active_nhs.size()]).to_string().c_str(), 
                                exp_bucket_size +1, num_nhs_with_one_more -1);
                        bank_member_change.active_nhs.erase(bank_member_change.active_nhs.begin() + 
                            (i % bank_member_change.active_nhs.size()));
                        num_nhs_with_one_more--;
                    }
                    else if(bank_fgnhg_map->at(round_robin_nh).size() > exp_bucket_size +1)
                    {
                        SWSS_LOG_WARN("Unexpected bucket size for nh %s, size %lu, exp_size %d",
                                round_robin_nh.to_string().c_str(), bank_fgnhg_map->at(round_robin_nh).size(),
                                exp_bucket_size + 1);
                    }
                }
            }

            bank_fgnhg_map->erase(bank_member_change.nhs_to_del[del_idx]);
            syncd_fg_route_entry->active_nexthops.erase(bank_member_change.nhs_to_del[del_idx]);
            del_idx++;
        }
    }

    if(add_idx < bank_member_change.nhs_to_add.size())
    {
        uint32_t total_nhs = (uint32_t)bank_member_change.active_nhs.size() +
                         (uint32_t)bank_member_change.nhs_to_add.size() - add_idx;
        uint32_t num_buckets_in_bank = 1+ fgNhgEntry->hash_bucket_indices[syncd_bank].end_index -
            fgNhgEntry->hash_bucket_indices[syncd_bank].start_index;
        uint32_t exp_bucket_size = num_buckets_in_bank/total_nhs;
        uint32_t num_nhs_with_one_more = (num_buckets_in_bank % total_nhs);
        uint32_t num_nhs_with_eq_to_exp = total_nhs - num_nhs_with_one_more;
        uint32_t add_nh_exp_bucket_size = exp_bucket_size;

        while(add_idx < bank_member_change.nhs_to_add.size())
        {
            (*bank_fgnhg_map)[bank_member_change.nhs_to_add[add_idx]] = 
                std::vector<uint32_t>();
            auto it = bank_member_change.active_nhs.begin();
            if(num_nhs_with_eq_to_exp > 0)
            {
                num_nhs_with_eq_to_exp--;
            }
            else
            {
                add_nh_exp_bucket_size = exp_bucket_size + 1;
                num_nhs_with_one_more--;
            }

            while(bank_fgnhg_map->at(bank_member_change.nhs_to_add[add_idx]).size() != add_nh_exp_bucket_size)
            {
                if(it == bank_member_change.active_nhs.end())
                {
                    it = bank_member_change.active_nhs.begin();
                }
                vector<uint32_t> *map_entry = &(bank_fgnhg_map->at(*it));
                if((*map_entry).size() <= 1)
                {
                    /* Case where the number of hash buckets for the nh is <= 1 */
                    SWSS_LOG_WARN("Next-hop %s has %d entries, either number of buckets were less or we hit a bug",
                            (*it).to_string().c_str(), ((int)(*map_entry).size()));
                    return false;
                }
                else
                {
                    uint32_t last_elem = map_entry->at((*map_entry).size() - 1);

                    if(!write_hash_bucket_change_to_sai(syncd_fg_route_entry, last_elem, 
                        nhopgroup_members_set[bank_member_change.nhs_to_add[add_idx]],
                        ipPrefix, bank_member_change.nhs_to_add[add_idx]))
                    {
                        return false;
                    }

                    (*bank_fgnhg_map)[bank_member_change.nhs_to_add[add_idx]].push_back(last_elem);
                    (*map_entry).erase((*map_entry).end() - 1);
                }
                /* Logic below ensure that # hash buckets assigned to a nh is equalized */
                if(num_nhs_with_one_more == 0)
                {
                    if(map_entry->size() == exp_bucket_size)
                    {
                        SWSS_LOG_INFO("%s reached %d, don't remove more buckets", it->to_string().c_str(), exp_bucket_size);
                        it = bank_member_change.active_nhs.erase(it);
                    }
                    else if(map_entry->size() < exp_bucket_size)
                    {
                        SWSS_LOG_WARN("Unexpected bucket size for nh %s, size %lu, exp_size %d",
                                it->to_string().c_str(), map_entry->size(), exp_bucket_size);
                        it++;
                    }
                    else
                    {
                        it++;
                    }
                }
                else
                {
                    if(map_entry->size() == exp_bucket_size +1)
                    {
                        SWSS_LOG_INFO("%s reached %d, don't remove more buckets num_nhs_with_one_more %d", 
                                it->to_string().c_str(), exp_bucket_size + 1, num_nhs_with_one_more -1);
                        it = bank_member_change.active_nhs.erase(it);
                        num_nhs_with_one_more--;
                    }
                    else if(map_entry->size() < exp_bucket_size)
                    {
                        SWSS_LOG_WARN("Unexpected bucket size for nh %s, size %lu, exp_size %d",
                                it->to_string().c_str(), map_entry->size(), exp_bucket_size + 1);
                        it++;
                    }
                    else
                    {
                        it++;
                    }
                }
            }
            syncd_fg_route_entry->active_nexthops.insert(bank_member_change.nhs_to_add[add_idx]);
            add_idx++;
        }
    }
    return true;
}


bool FgNhgOrch::set_inactive_bank_to_next_available_active_bank(FGNextHopGroupEntry *syncd_fg_route_entry, FgNhgEntry *fgNhgEntry,
        uint32_t bank, std::vector<Bank_Member_Changes> bank_member_changes,
        std::map<NextHopKey,sai_object_id_t> &nhopgroup_members_set, const IpPrefix &ipPrefix)
{
    SWSS_LOG_ENTER();

    uint32_t new_bank_idx = 0;
    for( ; new_bank_idx < bank_member_changes.size(); new_bank_idx++)
    {
        if(bank_member_changes[new_bank_idx].active_nhs.size() +
                bank_member_changes[new_bank_idx].nhs_to_add.size() != 0)
        {
            syncd_fg_route_entry->syncd_fgnhg_map[bank].clear();
            syncd_fg_route_entry->inactive_to_active_map[bank] = new_bank_idx;

            /* Create collated set of members which will be active in the bank */
            for(auto memb: bank_member_changes[new_bank_idx].nhs_to_add)
            {
                bank_member_changes[new_bank_idx].active_nhs.push_back(memb);
            }

            for(uint32_t i = fgNhgEntry->hash_bucket_indices[bank].start_index;
                i <= fgNhgEntry->hash_bucket_indices[bank].end_index; i++)
            {
                NextHopKey bank_nh_memb = bank_member_changes[new_bank_idx].
                         active_nhs[i % bank_member_changes[new_bank_idx].active_nhs.size()];

                if(!write_hash_bucket_change_to_sai(syncd_fg_route_entry, i,
                    nhopgroup_members_set[bank_nh_memb],ipPrefix, bank_nh_memb ))
                {
                    return false;
                }

                syncd_fg_route_entry->syncd_fgnhg_map[bank][bank_nh_memb].push_back(i);
            }
            break;
        }
    }

    if(new_bank_idx == bank_member_changes.size())
    {
        /* Case where there are no active banks */
        SWSS_LOG_NOTICE("All banks of FG next-hops are down for prefix %s",
                ipPrefix.to_string().c_str());

        /* This may occur when there are no neigh entries available any more
         * set route pointing to rif to allow for neigh resolution in kernel.
         * If route already points to rif when we are done.
         */
        if (!syncd_fg_route_entry->points_to_rif)
        {
            std::string interface_alias = syncd_fg_route_entry->nhg_key.getNextHops().begin()->alias;
            sai_object_id_t rif_next_hop_id = m_intfsOrch->getRouterIntfsId(interface_alias);
            if (rif_next_hop_id == SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_INFO("Failed to get rif next hop for %s", interface_alias.c_str());
                return false;
            }

            if (!modifyRoutesNextHopId(gVirtualRouterId, ipPrefix, rif_next_hop_id))
            {
                SWSS_LOG_ERROR("Failed to modify route nexthopid to rif");
                return false;
            }

            if (!remove_nhg(syncd_fg_route_entry))
            {
                return false;
            }
            syncd_fg_route_entry->points_to_rif = true;
            syncd_fg_route_entry->next_hop_group_id = rif_next_hop_id;

            // remove state_db entry
            m_stateWarmRestartRouteTable.del(ipPrefix.to_string());
            // Clear data structures
            syncd_fg_route_entry->syncd_fgnhg_map.clear();
            syncd_fg_route_entry->active_nexthops.clear();
            syncd_fg_route_entry->inactive_to_active_map.clear();
            syncd_fg_route_entry->nhopgroup_members.clear();
    }

    return true;
}


bool FgNhgOrch::set_inactive_bank_hash_bucket_changes(FGNextHopGroupEntry *syncd_fg_route_entry, FgNhgEntry *fgNhgEntry,
        uint32_t bank,std::vector<Bank_Member_Changes> &bank_member_changes, 
        std::map<NextHopKey,sai_object_id_t> &nhopgroup_members_set, const IpPrefix &ipPrefix)
{
    SWSS_LOG_ENTER();

    if(bank_member_changes[bank].nhs_to_add.size() > 0)
    {
        /* Previously inactive bank now transistions to active */
        syncd_fg_route_entry->syncd_fgnhg_map[bank].clear();
        for(uint32_t i = fgNhgEntry->hash_bucket_indices[bank].start_index;
                i <= fgNhgEntry->hash_bucket_indices[bank].end_index; i++)
        {
            NextHopKey bank_nh_memb = bank_member_changes[bank].
                nhs_to_add[i % bank_member_changes[bank].nhs_to_add.size()];

            if(!write_hash_bucket_change_to_sai(syncd_fg_route_entry, i, 
                  nhopgroup_members_set[bank_nh_memb], ipPrefix, bank_nh_memb))
            {
                return false;
            }

            syncd_fg_route_entry->syncd_fgnhg_map[bank][bank_nh_memb].push_back(i);
            syncd_fg_route_entry->active_nexthops.insert(bank_nh_memb);
        }
        syncd_fg_route_entry->inactive_to_active_map[bank] = bank;
        SWSS_LOG_NOTICE("Bank# %d of FG next-hops is up for prefix %s", bank,
                ipPrefix.to_string().c_str());
    }
    else if(bank_member_changes[bank].nhs_to_del.size() > 0)
    {
        /* Previously active bank now transistions to inactive */
        if(!set_inactive_bank_to_next_available_active_bank(syncd_fg_route_entry, fgNhgEntry,
                    bank, bank_member_changes, nhopgroup_members_set, ipPrefix))
        {
            SWSS_LOG_INFO("Failed to map to active_bank and set nh in SAI");
            return false;
        }

        for(auto memb: bank_member_changes[bank].nhs_to_del)
        {
            syncd_fg_route_entry->active_nexthops.erase(memb);
        }
        SWSS_LOG_NOTICE("Bank# %d of FG next-hops is down for prefix %s", bank,
                ipPrefix.to_string().c_str());
    }
    else
    {
        /* Previously inactive bank remains inactive */
        uint32_t active_bank = syncd_fg_route_entry->inactive_to_active_map[bank];
        if(bank_member_changes[active_bank].active_nhs.size() == 0)
        {
            if(!set_inactive_bank_to_next_available_active_bank(syncd_fg_route_entry, fgNhgEntry,
                        bank, bank_member_changes, nhopgroup_members_set, ipPrefix))
            {
                SWSS_LOG_INFO("Failed to map to active_bank and set nh in SAI");
                return false;
            }
        }
        else
        {
            if(!set_active_bank_hash_bucket_changes(syncd_fg_route_entry, fgNhgEntry, 
                active_bank, bank, bank_member_changes, nhopgroup_members_set, ipPrefix))
            {
                SWSS_LOG_INFO("Failed set_active_bank_hash_bucket_changes");
                return false;
            }
        }
    }
    return true;
}


bool FgNhgOrch::compute_and_set_hash_bucket_changes(FGNextHopGroupEntry *syncd_fg_route_entry, 
        FgNhgEntry *fgNhgEntry, std::vector<Bank_Member_Changes> &bank_member_changes, 
        std::map<NextHopKey,sai_object_id_t> &nhopgroup_members_set,
        const IpPrefix &ipPrefix)
{
    SWSS_LOG_ENTER();

    for(uint32_t bank_idx = 0; bank_idx < bank_member_changes.size(); bank_idx++)
    {
        if(bank_member_changes[bank_idx].active_nhs.size() != 0 ||
                (bank_member_changes[bank_idx].nhs_to_add.size() != 0 &&
                 bank_member_changes[bank_idx].nhs_to_del.size() != 0))
        {
            if(!set_active_bank_hash_bucket_changes(syncd_fg_route_entry, fgNhgEntry, 
                        bank_idx, bank_idx, bank_member_changes, nhopgroup_members_set, ipPrefix))
            {
                return false;
            }
        }
        else
        {
            if(!set_inactive_bank_hash_bucket_changes(syncd_fg_route_entry, fgNhgEntry, 
                        bank_idx, bank_member_changes, nhopgroup_members_set, ipPrefix))
            {
                return false;
            }
        }
    }

    return true;
}


bool FgNhgOrch::set_new_nhg_members(FGNextHopGroupEntry &syncd_fg_route_entry, FgNhgEntry *fgNhgEntry, 
        std::vector<Bank_Member_Changes> &bank_member_changes, std::map<NextHopKey,sai_object_id_t> &nhopgroup_members_set,
        const IpPrefix &ipPrefix)
{
    SWSS_LOG_ENTER();

    sai_status_t status;
    bool is_warm_reboot = false;
    auto nexthopsMap = m_recoveryMap.find(ipPrefix.to_string());
    for(uint32_t i = 0; i < fgNhgEntry->hash_bucket_indices.size(); i++) 
    {
        uint32_t bank = i;
        syncd_fg_route_entry.inactive_to_active_map[bank] = bank;
        if(i + 1 > syncd_fg_route_entry.syncd_fgnhg_map.size())
        {
            syncd_fg_route_entry.syncd_fgnhg_map.push_back(FGNextHopGroupMap());
        }

        if(bank_member_changes[i].nhs_to_add.size() == 0)
        {
            /* Case where bank is empty */
            for(uint32_t active_bank = 0; active_bank < bank_member_changes.size(); active_bank++)
            {
                if(bank_member_changes[active_bank].nhs_to_add.size() != 0)
                {
                    bank = active_bank;
                    syncd_fg_route_entry.inactive_to_active_map[i] = active_bank;
                    break;
                }
            }

            SWSS_LOG_NOTICE("Bank# %d of FG next-hops is down for prefix %s", i,
                    ipPrefix.to_string().c_str());
        } 

        if(bank_member_changes[bank].nhs_to_add.size() == 0)
        {
            /* Case where all banks are empty, we let retry logic(upon rv false) take care of this scenario */
            SWSS_LOG_INFO("Found no next-hops to add, skipping");
            return false;
        }

        // recover state before warm reboot
        if (nexthopsMap != m_recoveryMap.end())
        {
            is_warm_reboot = true;
        }

        for(uint32_t j = fgNhgEntry->hash_bucket_indices[i].start_index;
                j <= fgNhgEntry->hash_bucket_indices[i].end_index; j++)
        {
            NextHopKey bank_nh_memb;
            if (is_warm_reboot)
            {
                bank_nh_memb = nexthopsMap->second[j];
                SWSS_LOG_INFO("Recovering nexthop %s with bucket %d", bank_nh_memb.ip_address.to_string().c_str(), j);
                // case nhps in bank are all down
                if (fgNhgEntry->next_hops[bank_nh_memb.ip_address].bank != i)
                {
                    syncd_fg_route_entry.inactive_to_active_map[i] = fgNhgEntry->next_hops[bank_nh_memb.ip_address].bank;
                }
            }
            else
            {
                bank_nh_memb = bank_member_changes[bank].nhs_to_add[j %
                    bank_member_changes[bank].nhs_to_add.size()];
            }

            // Create a next hop group member
            sai_attribute_t nhgm_attr;
            vector<sai_attribute_t> nhgm_attrs;
            nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
            nhgm_attr.value.oid = syncd_fg_route_entry.next_hop_group_id;
            nhgm_attrs.push_back(nhgm_attr);

            nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
            nhgm_attr.value.oid = nhopgroup_members_set[bank_nh_memb];
            nhgm_attrs.push_back(nhgm_attr);

            nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_INDEX;
            nhgm_attr.value.s32 = j;
            nhgm_attrs.push_back(nhgm_attr);

            sai_object_id_t next_hop_group_member_id;
            status = sai_next_hop_group_api->create_next_hop_group_member(
                                                            &next_hop_group_member_id,
                                                            gSwitchId,
                                                            (uint32_t)nhgm_attrs.size(),
                                                            nhgm_attrs.data());

            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to create next hop group %" PRIx64 " member %" PRIx64 ": %d\n",
                     syncd_fg_route_entry.next_hop_group_id, next_hop_group_member_id, status);

                if(!remove_nhg(&syncd_fg_route_entry))
                {
                    SWSS_LOG_ERROR("Failed to clean-up after next-hop member creation failure");
                }

                return false;
            }

            set_state_db_route_entry(ipPrefix, j, bank_nh_memb);
            syncd_fg_route_entry.syncd_fgnhg_map[i][bank_nh_memb].push_back(j);
            syncd_fg_route_entry.active_nexthops.insert(bank_nh_memb);
            syncd_fg_route_entry.nhopgroup_members.push_back(next_hop_group_member_id);
            gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
        }
    }

    if (is_warm_reboot)
    {
        m_recoveryMap.erase(nexthopsMap);
    }

    return true;
}


bool FgNhgOrch::createFgNhg(sai_object_id_t vrf_id, const IpPrefix &ipPrefix, FGNextHopGroupEntry &syncd_fg_route_entry, FgNhgEntry *fgNhgEntry,
        std::vector<Bank_Member_Changes> &bank_member_changes, std::map<NextHopKey,sai_object_id_t> &nhopgroup_members_set)
{
    if(gRouteOrch->getNextHopGroupCount() >= gRouteOrch->getMaxNextHopGroupCount())
    {
        SWSS_LOG_DEBUG("Failed to create new next hop group. \
                    Reached maximum number of next hop groups.");
        return false;
    }

    string platform = getenv("platform") ? getenv("platform") : "";

    sai_attribute_t nhg_attr;
    vector<sai_attribute_t> nhg_attrs;

    nhg_attr.id = SAI_NEXT_HOP_GROUP_ATTR_TYPE;
    nhg_attr.value.s32 = SAI_NEXT_HOP_GROUP_TYPE_FINE_GRAIN_ECMP;
    nhg_attrs.push_back(nhg_attr);

    nhg_attr.id = SAI_NEXT_HOP_GROUP_ATTR_CONFIGURED_SIZE;
    nhg_attr.value.s32 = fgNhgEntry->configured_bucket_size;
    nhg_attrs.push_back(nhg_attr);

    sai_object_id_t next_hop_group_id;
    sai_status_t status = sai_next_hop_group_api->create_next_hop_group(&next_hop_group_id,
                                                                    gSwitchId,
                                                                    (uint32_t)nhg_attrs.size(),
                                                                    nhg_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create FG next hop group for prefix %s, rv:%d",
                       ipPrefix.to_string().c_str(), status);
        return false;
    }

    gRouteOrch->incNextHopGroupCount();
    gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);

    if(platform == VS_PLATFORM_SUBSTRING)
    {
       /* TODO: need implementation for SAI_NEXT_HOP_GROUP_ATTR_REAL_SIZE */ 
        fgNhgEntry->real_bucket_size = fgNhgEntry->configured_bucket_size;
    }
    else
    {
        nhg_attr.id = SAI_NEXT_HOP_GROUP_ATTR_REAL_SIZE;
        nhg_attr.value.u32 = 0;
        status = sai_next_hop_group_api->get_next_hop_group_attribute(next_hop_group_id, 1, &nhg_attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to query next hop group for prefix %s SAI_NEXT_HOP_GROUP_ATTR_REAL_SIZE, rv:%d",
                       ipPrefix.to_string().c_str(), status);
            return false;
        }
        fgNhgEntry->real_bucket_size = nhg_attr.value.u32;
    }

    calculate_bank_hash_bucket_start_indices(fgNhgEntry);

    SWSS_LOG_NOTICE("fgnhgorch created FG next hop group for prefix %s of size %d", ipPrefix.to_string().c_str(), fgNhgEntry->real_bucket_size);

    syncd_fg_route_entry.next_hop_group_id = next_hop_group_id;

    if(!set_new_nhg_members(syncd_fg_route_entry, fgNhgEntry, bank_member_changes, nhopgroup_members_set, ipPrefix))
    {
        return false;
    }

    syncd_fg_route_entry.points_to_rif = false;

    return true;
}


bool FgNhgOrch::isRouteFineGrained(sai_object_id_t vrf_id, const IpPrefix &ipPrefix, const NextHopGroupKey &nextHops)
{
    SWSS_LOG_ENTER();
 
    if (!isFineGrainedConfigured || (vrf_id != gVirtualRouterId))
    {
        return false;
    }

    FgNhgEntry *fgNhgEntry = 0;
    set<NextHopKey> next_hop_set = nextHops.getNextHops();
    auto prefix_entry = m_fgNhgPrefixes.find(ipPrefix);
    if (prefix_entry == m_fgNhgPrefixes.end())
    {
        for (NextHopKey nhk : next_hop_set)
        {
            auto member_entry = m_fgNhgNexthops.find(nhk.ip_address);
            if (member_entry == m_fgNhgNexthops.end())
            {
                if (fgNhgEntry)
                {
                    SWSS_LOG_WARN("Route %s:%s has some FG nhs, but %s is not, route is defaulted to non-fine grained ECMP",
                                ipPrefix.to_string().c_str(), nextHops.to_string().c_str(), nhk.to_string().c_str());
                }
                return false;
            }

            if (!fgNhgEntry)
            {
                fgNhgEntry = member_entry->second;
            }
            else
            {
                /* Case where fgNhgEntry is alredy found via previous nexthop
                 * We validate the it belongs to the same next-hop group set
                 */
                if (fgNhgEntry != member_entry->second)
                {
                    SWSS_LOG_INFO("FG nh found across different FG_NH groups: %s expected %s, actual %s", 
                        nhk.to_string().c_str(), fgNhgEntry->fgNhg_name.c_str(), member_entry->second->fgNhg_name.c_str());
                    return false;
                }
            }
        }
    }
    return true;
}


bool FgNhgOrch::syncdContainsFgNhg(sai_object_id_t vrf_id, const IpPrefix &ipPrefix)
{
    if (!isFineGrainedConfigured || (vrf_id != gVirtualRouterId))
    {
        return false;
    }

    auto it_route_table = m_syncdFGRouteTables.find(vrf_id);
    if (it_route_table == m_syncdFGRouteTables.end())
    {
        return false;
    }

    auto it_route = it_route_table->second.find(ipPrefix);
    if (it_route == it_route_table->second.end())
    {
        return false;
    }
    return true;
}


bool FgNhgOrch::setFgNhg(sai_object_id_t vrf_id, const IpPrefix &ipPrefix, const NextHopGroupKey &nextHops,
                                    sai_object_id_t &next_hop_id, bool &isNextHopIdChanged)
{
    SWSS_LOG_ENTER();

    /* default isNextHopIdChanged to false so that sai route is unaffected
     * when we return early with success */
    isNextHopIdChanged = false;
    FgNhgEntry *fgNhgEntry = 0;
    set<NextHopKey> next_hop_set = nextHops.getNextHops();
    auto prefix_entry = m_fgNhgPrefixes.find(ipPrefix);
    if (prefix_entry != m_fgNhgPrefixes.end())
    {
        fgNhgEntry = prefix_entry->second;
    }
    else
    {
        for (NextHopKey nhk : next_hop_set)
        {
            auto member_entry = m_fgNhgNexthops.find(nhk.ip_address);
            if (member_entry == m_fgNhgNexthops.end())
            {
                SWSS_LOG_ERROR("fgNhgOrch got a route addition %s:%s for non-configured FG ECMP entry",
                                    ipPrefix.to_string().c_str(), nextHops.to_string().c_str());
                return false;
            }
            fgNhgEntry = member_entry->second;
            break;
        }
    }

    if (m_syncdFGRouteTables.find(vrf_id) != m_syncdFGRouteTables.end() &&
        m_syncdFGRouteTables.at(vrf_id).find(ipPrefix) != m_syncdFGRouteTables.at(vrf_id).end() &&
        m_syncdFGRouteTables.at(vrf_id).at(ipPrefix).nhg_key == nextHops)
    {
        return true;
    }

    if (m_syncdFGRouteTables.find(vrf_id) == m_syncdFGRouteTables.end())
    {
        m_syncdFGRouteTables.emplace(vrf_id, FGRouteTable());
        m_vrfOrch->increaseVrfRefCount(vrf_id);
    }

    std::map<NextHopKey,sai_object_id_t> nhopgroup_members_set;
    auto syncd_fg_route_entry_it = m_syncdFGRouteTables.at(vrf_id).find(ipPrefix);
    bool next_hop_to_add = false;

    /* Default init with # of banks */
    std::vector<Bank_Member_Changes> bank_member_changes(
            fgNhgEntry->hash_bucket_indices.size(), Bank_Member_Changes());
    if(fgNhgEntry->hash_bucket_indices.size() == 0)
    {
        /* Only happens the 1st time when hash_bucket_indices are not inited
         */
        for (auto it : fgNhgEntry->next_hops)
        {
            while(bank_member_changes.size() <= it.second.bank)
            {
                bank_member_changes.push_back(Bank_Member_Changes());
            }
        }
    }

    /* Assert each IP address exists in m_syncdNextHops table,
     * and add the corresponding next_hop_id to next_hop_ids. */
    for (NextHopKey nhk : next_hop_set)
    {
        auto nexthop_entry = fgNhgEntry->next_hops.find(nhk.ip_address);
        if (!m_neighOrch->hasNextHop(nhk))
        {
            SWSS_LOG_NOTICE("Failed to get next hop %s:%s in neighorch",
                    nhk.to_string().c_str(), nextHops.to_string().c_str());
            continue;
        }
        else if (nexthop_entry == fgNhgEntry->next_hops.end())
        {
            SWSS_LOG_WARN("Could not find next-hop %s in Fine Grained next-hop group entry for prefix %s, skipping",
                    nhk.to_string().c_str(), fgNhgEntry->fgNhg_name.c_str());
            continue;
        }
        else if (!(nexthop_entry->second.link.empty()) &&
                nexthop_entry->second.link_oper_state == LINK_DOWN)
        {
            SWSS_LOG_NOTICE("Tracked link %s associated with nh %s is down",
                    nexthop_entry->second.link.c_str(), nhk.to_string().c_str());
            continue;
        }
        else if (m_neighOrch->isNextHopFlagSet(nhk, NHFLAGS_IFDOWN))
        {
            SWSS_LOG_NOTICE("Next hop %s in %s is down, skipping",
                    nhk.to_string().c_str(), nextHops.to_string().c_str());
            continue;
        }

        if(syncd_fg_route_entry_it == m_syncdFGRouteTables.at(vrf_id).end())
        {
            bank_member_changes[fgNhgEntry->next_hops[nhk.ip_address].bank].
                nhs_to_add.push_back(nhk);
            next_hop_to_add = true;
        }
        else 
        {
            FGNextHopGroupEntry *syncd_fg_route_entry = &(syncd_fg_route_entry_it->second);
            if(syncd_fg_route_entry->active_nexthops.find(nhk) == 
                syncd_fg_route_entry->active_nexthops.end())
            {
                bank_member_changes[fgNhgEntry->next_hops[nhk.ip_address].bank].
                    nhs_to_add.push_back(nhk);
                next_hop_to_add = true;
            }
        }

        sai_object_id_t nhid = m_neighOrch->getNextHopId(nhk);
        nhopgroup_members_set[nhk] = nhid;
    }


    if(syncd_fg_route_entry_it != m_syncdFGRouteTables.at(vrf_id).end())
    {
        /* Route exists and nh was associated in the past */
        FGNextHopGroupEntry *syncd_fg_route_entry = &(syncd_fg_route_entry_it->second);

        if (syncd_fg_route_entry->points_to_rif)
        {
            if (next_hop_to_add)
            {
                isNextHopIdChanged = true;
                if (!createFgNhg(vrf_id, ipPrefix, *syncd_fg_route_entry, fgNhgEntry, bank_member_changes, nhopgroup_members_set))
                {
                    return false;
                } 
            }
        }
        else
        {
            /* Update FG ECMP group in SAI */
            for(auto nhk : syncd_fg_route_entry->active_nexthops)
            {
                if(nhopgroup_members_set.find(nhk) == nhopgroup_members_set.end())
                {
                    bank_member_changes[fgNhgEntry->next_hops[nhk.ip_address].bank].
                        nhs_to_del.push_back(nhk);
                }
                else
                {
                    bank_member_changes[fgNhgEntry->next_hops[nhk.ip_address].bank].
                        active_nhs.push_back(nhk);
                }
            }

            if(!compute_and_set_hash_bucket_changes(syncd_fg_route_entry, fgNhgEntry, bank_member_changes, 
                    nhopgroup_members_set, ipPrefix))
            {
                return false;
            }
        }
    }
    else
    {
        /* New route + nhg addition */
        isNextHopIdChanged = true;
        FGNextHopGroupEntry syncd_fg_route_entry;
        if (next_hop_to_add)
        {
            if (!createFgNhg(vrf_id, ipPrefix, syncd_fg_route_entry, fgNhgEntry, bank_member_changes, nhopgroup_members_set))
            {
                return false;
            }
        }
        else
        {
            sai_object_id_t rif_next_hop_id = m_intfsOrch->getRouterIntfsId(next_hop_set.begin()->alias);
            if (rif_next_hop_id == SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_INFO("Failed to get rif next hop %s for %s",
                        nextHops.to_string().c_str(), ipPrefix.to_string().c_str());
                return false;
            }

            syncd_fg_route_entry.next_hop_group_id = rif_next_hop_id;
            syncd_fg_route_entry.points_to_rif = true;
        }

        m_syncdFGRouteTables[vrf_id][ipPrefix] = syncd_fg_route_entry;
    }
    m_syncdFGRouteTables[vrf_id][ipPrefix].nhg_key = nextHops; 

    for(uint32_t bank_idx = 0; bank_idx < bank_member_changes.size(); bank_idx++)
    {
        for(auto nh : bank_member_changes[bank_idx].nhs_to_add)
        {
            m_neighOrch->increaseNextHopRefCount(nh);
            SWSS_LOG_INFO("FG nh %s for prefix %s is up",
                    nh.to_string().c_str(), ipPrefix.to_string().c_str());
        }

        for(auto nh : bank_member_changes[bank_idx].nhs_to_del)
        {
            m_neighOrch->decreaseNextHopRefCount(nh);
            SWSS_LOG_INFO("FG nh %s for prefix %s is down",
                    nh.to_string().c_str(), ipPrefix.to_string().c_str());
        }
    }

    next_hop_id =  m_syncdFGRouteTables[vrf_id][ipPrefix].next_hop_group_id;
    return true;
}


bool FgNhgOrch::removeFgNhg(sai_object_id_t vrf_id, const IpPrefix &ipPrefix)
{
    SWSS_LOG_ENTER();

    if (!isFineGrainedConfigured)
    {
        return true;
    }

    auto it_route_table = m_syncdFGRouteTables.find(vrf_id);
    if (it_route_table == m_syncdFGRouteTables.end())
    {
        SWSS_LOG_INFO("Failed to find route table, vrf_id 0x%" PRIx64 "\n", vrf_id);
        return true;
    }

    auto it_route = it_route_table->second.find(ipPrefix);
    if (it_route == it_route_table->second.end())
    {
        SWSS_LOG_INFO("Failed to find route entry, vrf_id 0x%" PRIx64 ", prefix %s\n", vrf_id,
                ipPrefix.to_string().c_str());
        return true;
    }

    FGNextHopGroupEntry *syncd_fg_route_entry = &(it_route->second);
    if (!syncd_fg_route_entry->points_to_rif)
    {
        if(!remove_nhg(syncd_fg_route_entry))
        {
            SWSS_LOG_ERROR("Failed to clean-up fine grained ECMP SAI group");
            return false;
        }

        for(auto nh : syncd_fg_route_entry->active_nexthops)
        {
            m_neighOrch->decreaseNextHopRefCount(nh);
        }

        // remove state_db entry
        m_stateWarmRestartRouteTable.del(ipPrefix.to_string());
    }

    it_route_table->second.erase(it_route);
    if (it_route_table->second.size() == 0)
    {
        m_syncdFGRouteTables.erase(vrf_id);
        m_vrfOrch->decreaseVrfRefCount(vrf_id);
    }
    SWSS_LOG_NOTICE("All banks of FG next-hops are down for prefix %s",
            ipPrefix.to_string().c_str());

    return true;
}


void FgNhgOrch::cleanupIpInLinkToIpMap(const string &link, const IpAddress &ip, FgNhgEntry &fgNhg_entry)
{
    SWSS_LOG_ENTER();
    if (!link.empty())
    {
        auto link_entry = fgNhg_entry.links.find(link);
        if (link_entry == fgNhg_entry.links.end())
        {
            SWSS_LOG_WARN("Unexpected case where structs are out of sync for %s",
                    link.c_str());
            return;
        } 
        for (auto ip_it = begin(link_entry->second); ip_it != end(link_entry->second); ip_it++)
        {
            if (*ip_it == ip)
            {
                fgNhg_entry.links[link].erase(ip_it);
                break;
            }
        }
    }
}


bool FgNhgOrch::doTaskFgNhg(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();
    string op = kfvOp(t);
    string key = kfvKey(t);
    string fgNhg_name = key; 
    auto fgNhg_entry = m_FgNhgs.find(fgNhg_name);
    FGMatchMode match_mode = ROUTE_BASED;

    if (op == SET_COMMAND)
    {
        uint32_t bucket_size = 0;

        for (auto i : kfvFieldsValues(t))
        {
            if (fvField(i) == "bucket_size")
            {
                bucket_size = stoi(fvValue(i));
            }
            else if (fvField(i) == "match_mode")
            {
                if (fvValue(i) == "nexthop-based")
                {
                    match_mode = NEXTHOP_BASED;
                }
                else if (fvValue(i) != "route-based")
                {
                    SWSS_LOG_WARN("Received unsupported match_mode %s, defaulted to route-based",
                                    fvValue(i).c_str());
                }
            }
        }

        if(bucket_size == 0)
        {
            SWSS_LOG_ERROR("Received bucket_size which is 0 for key %s", kfvKey(t).c_str());
            return true;
        }

        if(fgNhg_entry != m_FgNhgs.end()) 
        {
            SWSS_LOG_WARN("FG_NHG %s already exists, ignoring", fgNhg_name.c_str());
        }
        else
        {
            FgNhgEntry fgNhgEntry;
            fgNhgEntry.configured_bucket_size = bucket_size;
            fgNhgEntry.fgNhg_name = fgNhg_name;
            fgNhgEntry.match_mode = match_mode;
            SWSS_LOG_NOTICE("Added new FG_NHG entry with bucket_size %d, match_mode: %'" PRIu8, 
                    bucket_size, match_mode);
            isFineGrainedConfigured = true;
            m_FgNhgs[fgNhg_name] = fgNhgEntry;
        }
    }
    else if (op == DEL_COMMAND)
    {
        if(fgNhg_entry == m_FgNhgs.end())
        {
            SWSS_LOG_INFO("%s: Received delete call for non-existent entry %s",
                    __FUNCTION__, fgNhg_name.c_str());
        }
        else 
        {
            /* delete all associated FG_NHG and SAI objects */
            if (fgNhg_entry->second.prefixes.size() == 0 && fgNhg_entry->second.next_hops.size() == 0)
            {
                m_FgNhgs.erase(fgNhg_entry);
                assert(m_FgNhgs.find(fgNhg_name) == fgNhgPrefixes.end());
                SWSS_LOG_INFO("%s: Received delete call for valid entry with no further dependencies, deleting %s",
                        __FUNCTION__, fgNhg_name.c_str());
            }
            else
            {
                SWSS_LOG_INFO("Child Prefix/Member entries are still associated with this FG_NHG %s", 
                        fgNhg_name.c_str());
                return false;
            }
            if (m_FgNhgs.size() == 0)
            {
                isFineGrainedConfigured = false;
            }
        }
    }
    return true;
}


bool FgNhgOrch::doTaskFgNhg_prefix(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();
    string op = kfvOp(t);
    string key = kfvKey(t);
    IpPrefix ip_prefix = IpPrefix(key);
    auto prefix_entry = m_fgNhgPrefixes.find(ip_prefix);

    if (op == SET_COMMAND)
    {
        if(prefix_entry != m_fgNhgPrefixes.end())
        {
            SWSS_LOG_INFO("%s FG_NHG prefix already exists", __FUNCTION__);
            return true;
        }

        string fgNhg_name = "";
        for (auto i : kfvFieldsValues(t))
        {
            if (fvField(i) == "FG_NHG")
            {
                fgNhg_name = fvValue(i);
            }
        }
        if(fgNhg_name == "")
        {
            SWSS_LOG_ERROR("Received FG_NHG with empty name for key %s", kfvKey(t).c_str());
            return true;
        }

        auto fgNhg_entry = m_FgNhgs.find(fgNhg_name);
        if(fgNhg_entry == m_FgNhgs.end())
        {
            SWSS_LOG_INFO("%s FG_NHG entry not received yet, continue", __FUNCTION__);
            return false;
        }

        if (fgNhg_entry->second.match_mode == NEXTHOP_BASED)
        {
            SWSS_LOG_NOTICE("FG_NHG %s is configured as nexthop_based: FG_NHG_PREFIX is a no-op",
                    fgNhg_name.c_str());
            return true;
        }

        fgNhg_entry->second.prefixes.push_back(ip_prefix);
        m_fgNhgPrefixes[ip_prefix] = &(fgNhg_entry->second);

        /* Transition route from regular to Fine Grained ECMP */
        sai_object_id_t vrf_id = gVirtualRouterId;
        auto route_table_entry = gRouteOrch->getSyncdRoutes().find(vrf_id);
        NextHopGroupKey nhg;
        if (route_table_entry == gRouteOrch->getSyncdRoutes().end())
        {
            SWSS_LOG_INFO("Failed to find route table, vrf_id 0x%lx\n", vrf_id);
        }
        else
        {
            /* ipprefix already exist in routeorch */
            SWSS_LOG_INFO("Find entry in route table, vrf_id 0x%lx\n", vrf_id);
            auto it_route = route_table_entry->second.find(ip_prefix);
            if (it_route != route_table_entry->second.end())
            {
                nhg = it_route->second;
                if(!gRouteOrch->addRoute(vrf_id, ip_prefix, nhg))
                {
                    SWSS_LOG_INFO("Failed to add fg route, %s:%s", 
                        ip_prefix.to_string().c_str(), nhg.to_string().c_str());
                    /* Cleanup struct due to failure */
                    for (uint32_t i = 0; i < prefix_entry->second->prefixes.size(); i++)
                    {
                        if (prefix_entry->second->prefixes[i] == ip_prefix)
                        {
                            prefix_entry->second->prefixes.erase(prefix_entry->second->prefixes.begin() + i);
                            SWSS_LOG_INFO("%s FG_NHG prefix %s is deleted from group %s",
                                    __FUNCTION__, ip_prefix.to_string().c_str(), m_fgNhgPrefixes[ip_prefix]->fgNhg_name.c_str());
                            break;
                        }
                    }
                    m_fgNhgPrefixes.erase(ip_prefix);
                    return false;
                }
            }
        }

        SWSS_LOG_INFO("%s FG_NHG added for group %s, prefix %s",
                __FUNCTION__, fgNhg_name.c_str(), ip_prefix.to_string().c_str());
    }
    else if (op == DEL_COMMAND)
    {
        if(prefix_entry == m_fgNhgPrefixes.end())
        {
            SWSS_LOG_INFO("%s FG_NHG prefix doesn't exists, ignore", __FUNCTION__);
            return true;
        }

        /* search and delete local structure */
        for (uint32_t i = 0; i < prefix_entry->second->prefixes.size(); i++)
        {
            if (prefix_entry->second->prefixes[i] == ip_prefix)
            {
                prefix_entry->second->prefixes.erase(prefix_entry->second->prefixes.begin() + i);
                SWSS_LOG_INFO("%s FG_NHG prefix %s is deleted from group %s",
                        __FUNCTION__, ip_prefix.to_string().c_str(), m_fgNhgPrefixes[ip_prefix]->fgNhg_name.c_str());
                break;
            }
        }
        m_fgNhgPrefixes.erase(ip_prefix);

        /* Transition route to regular ECMP */
        sai_object_id_t vrf_id = gVirtualRouterId;
        NextHopGroupKey nhg;
        if (m_syncdFGRouteTables.find(vrf_id) != m_syncdFGRouteTables.end() &&
                m_syncdFGRouteTables.at(vrf_id).find(ip_prefix) != m_syncdFGRouteTables.at(vrf_id).end())
        {
            nhg = m_syncdFGRouteTables.at(vrf_id).at(ip_prefix).nhg_key;
            if(!gRouteOrch->addRoute(vrf_id, ip_prefix, nhg))
            {
                SWSS_LOG_INFO("Failed to add regular ecmp route, %s:%s", 
                    ip_prefix.to_string().c_str(), nhg.to_string().c_str());
                prefix_entry->second->prefixes.push_back(ip_prefix);
                m_fgNhgPrefixes[ip_prefix] = prefix_entry->second;

                return false;
            }

            SWSS_LOG_INFO("%s FG_NHG prefix %s is transition to regular ECMP",
                    __FUNCTION__, ip_prefix.to_string().c_str());
        }
    }
    return true;
}


bool FgNhgOrch::doTaskFgNhg_member(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();
    string op = kfvOp(t);
    string key = kfvKey(t);
    IpAddress next_hop = IpAddress(key);
    NextHopKey nhk(next_hop.to_string());
    bool link_oper = LINK_UP;

    if (op == SET_COMMAND)
    {
        string fg_nhg_name = "";
        uint32_t bank = 0;
        string link = "";
        for (auto i : kfvFieldsValues(t))
        {
            if (fvField(i) == "FG_NHG")
            {
                fg_nhg_name = fvValue(i);
            }
            else if (fvField(i) == "bank")
            {
                bank = stoi(fvValue(i));
            }
            else if (fvField(i) == "link")
            {
                link = fvValue(i);
            }
        }
        if (fg_nhg_name.empty())
        {
            SWSS_LOG_ERROR("Received FG_NHG with empty name for key %s", kfvKey(t).c_str());
            return true;
        }

        auto fgNhg_entry = m_FgNhgs.find(fg_nhg_name);
        if (fgNhg_entry == m_FgNhgs.end())
        {
            SWSS_LOG_INFO("FG_NHG entry not received yet, continue");
            return false;
        }
        else
        {
            /* skip addition if next-hop already exists */
            if (fgNhg_entry->second.next_hops.find(next_hop) != fgNhg_entry->second.next_hops.end())
            {
                SWSS_LOG_INFO("FG_NHG member %s already exists for %s, skip",
                        next_hop.to_string().c_str(), fg_nhg_name.c_str());
                return true;
            }
            FGNextHopInfo fg_nh_info = {};
            fg_nh_info.bank = bank;

            if (!link.empty())
            {
                /* Identify link oper state for initialization */
                Port p;
                if (!gPortsOrch->getPort(link, p))
                {
                    SWSS_LOG_WARN("FG_NHG member %s added to %s with non-existent link %s, link mapping skipped",
                            next_hop.to_string().c_str(), fg_nhg_name.c_str(), link.c_str());
                }
                else
                {
                    link_oper = LINK_DOWN; /* Default operational state is down */
                    fg_nh_info.link = link;
                    if (p.m_oper_status == SAI_PORT_OPER_STATUS_UP)
                    {
                        link_oper = LINK_UP;
                    }
                    auto link_info = fgNhg_entry->second.links.find(link);
                    fg_nh_info.link_oper_state = link_oper;

                    if (link_info != fgNhg_entry->second.links.end())
                    {
                        link_info->second.push_back(next_hop);
                    }
                    else
                    {
                        std::vector<IpAddress> ips;
                        ips.push_back(next_hop);
                        fgNhg_entry->second.links[link] = ips;
                    }
                    SWSS_LOG_INFO("Added link %s to ip %s map", link.c_str(), key.c_str());
                }
            }

            fgNhg_entry->second.next_hops[next_hop] = fg_nh_info;

            if (fgNhg_entry->second.match_mode == NEXTHOP_BASED)
            {
                SWSS_LOG_NOTICE("Add member %s as NEXTHOP_BASED", next_hop.to_string().c_str());
                m_fgNhgNexthops[next_hop] = &(fgNhg_entry->second);
            }

            /* query and check the next hop is valid in neighOrcch */
            if (!m_neighOrch->hasNextHop(nhk))
            {
                SWSS_LOG_INFO("Nexthop %s is not resolved yet", nhk.to_string().c_str());
            }
            else if (link_oper)
            {
                /* add next-hop into SAI group if associated link is up/no link associated with this nh */
                if (!validNextHopInNextHopGroup(nhk))
                {
                    cleanupIpInLinkToIpMap(link, next_hop, fgNhg_entry->second);
                    fgNhg_entry->second.next_hops.erase(next_hop);
                    m_fgNhgNexthops.erase(next_hop);
                    SWSS_LOG_INFO("Failing validNextHopInNextHopGroup for %s", nhk.to_string().c_str());
                    return false;
                }
            }

            SWSS_LOG_INFO("FG_NHG member added for group %s, next-hop %s",
                    fgNhg_entry->second.fgNhg_name.c_str(), next_hop.to_string().c_str());
        }
    }
    else if (op == DEL_COMMAND)
    {
        /* remove next hop from SAI group if its a resolved nh which is programmed to SAI*/
        if (m_neighOrch->hasNextHop(nhk))
        {
            if (!invalidNextHopInNextHopGroup(nhk))
            {
                return false;
            }
        }

        SWSS_LOG_INFO("%s FG_NHG member removed for SAI group, next-hop %s",
                __FUNCTION__, next_hop.to_string().c_str());

        /* remove next-hop in fgnhg entry*/
        for (auto fgnhg_it = m_FgNhgs.begin(); fgnhg_it != m_FgNhgs.end(); ++fgnhg_it)
        {
            auto it = fgnhg_it->second.next_hops.find(next_hop);
            if (it != fgnhg_it->second.next_hops.end())
            {
                string link = it->second.link;
                cleanupIpInLinkToIpMap(link, next_hop, fgnhg_it->second);

                fgnhg_it->second.next_hops.erase(it);
                SWSS_LOG_INFO("FG_NHG member removed for group %s, next-hop %s",
                        fgnhg_it->second.fgNhg_name.c_str(), next_hop.to_string().c_str());
            }
        }
        m_fgNhgNexthops.erase(next_hop);
    }
    return true;
}
        

void FgNhgOrch::doTask(Consumer& consumer) {
    SWSS_LOG_ENTER();

    const string & table_name = consumer.getTableName();
    auto it = consumer.m_toSync.begin();
    bool entry_handled = true;

    while (it != consumer.m_toSync.end())
    {
        auto t = it->second;
        if(table_name == CFG_FG_NHG)
        {
            entry_handled = doTaskFgNhg(t);
        }
        else if(table_name == CFG_FG_NHG_PREFIX)
        {
            entry_handled = doTaskFgNhg_prefix(t);
        }
        else if(table_name == CFG_FG_NHG_MEMBER)
        {
            entry_handled = doTaskFgNhg_member(t);
        }
        else
        {
            entry_handled = true;
            SWSS_LOG_ERROR("%s Unknown table : %s", __FUNCTION__,table_name.c_str());
        }

        if (entry_handled)
        {
            consumer.m_toSync.erase(it++);
        }
        else
        {
            it++;
        }
    }
    return;
}
