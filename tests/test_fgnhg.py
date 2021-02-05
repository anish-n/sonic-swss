import os
import re
import time
import json
import pytest

from swsscommon import swsscommon

IF_TB = 'INTERFACE'
VLAN_TB = 'VLAN'
VLAN_MEMB_TB = 'VLAN_MEMBER'
VLAN_IF_TB = 'VLAN_INTERFACE'
VLAN_IF = 'VLAN_INTERFACE'
FG_NHG = 'FG_NHG'
FG_NHG_PREFIX = 'FG_NHG_PREFIX'
FG_NHG_MEMBER = 'FG_NHG_MEMBER'
ROUTE_TB = "ROUTE_TABLE"
ASIC_ROUTE_TB = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY"
ASIC_NHG_MEMB = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER"
ASIC_NH_TB = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP"

def create_entry(tbl, key, pairs):
    fvs = swsscommon.FieldValuePairs(pairs)
    tbl.set(key, fvs)
    time.sleep(1)


def create_entry_tbl(db, table, separator, key, pairs):
    tbl = swsscommon.Table(db, table)
    create_entry(tbl, key, pairs)

def remove_entry_tbl(db, table, key):
    tbl = swsscommon.Table(db, table)
    tbl._del(key)
    time.sleep(1)
    
def verify_programmed_nh_membs(db,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size):
    nh_memb_count = {}

    for key in nh_memb_exp_count:
        nh_memb_count[key] = 0

    nhg_member_tbl = swsscommon.Table(db, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER")
    memb_dict = {}

    for tbs in nhg_member_tbl.getKeys():
        (status, fvs) = nhg_member_tbl.get(tbs)
        assert status == True
        index = -1
        nh_oid = "0"
        memb_nhgid = "0"
        for fv in fvs:
            if fv[0] == "SAI_NEXT_HOP_GROUP_MEMBER_ATTR_INDEX":
                index = int(fv[1])
            elif fv[0] == "SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID":
                nh_oid = fv[1]
            elif fv[0] == "SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID":
                memb_nhgid = fv[1]
        assert memb_nhgid != "0"
        if memb_nhgid != nhgid:
            continue
        assert index != -1
        assert nh_oid != "0"
        assert nh_oid_map.get(nh_oid,"NULL") != "NULL"
        memb_dict[index] = nh_oid_map.get(nh_oid)
    idxs = [0]*bucket_size
    for idx,memb in memb_dict.items():
        nh_memb_count[memb] = 1 + nh_memb_count[memb]
        idxs[idx] = idxs[idx] + 1

    print nh_memb_count
    print "\n"
    print nh_memb_exp_count
    for key in nh_memb_exp_count:
        print key
        assert nh_memb_count[key] == nh_memb_exp_count[key]
    for idx in idxs:
        assert idx == 1

def shutdown_link(dvs, db, port):
    dvs.servers[port].runcmd("ip link set down dev eth0") == 0

    time.sleep(1)

    tbl = swsscommon.Table(db, "PORT_TABLE")
    (status, fvs) = tbl.get("Ethernet%d" % (port * 4))

    assert status == True

    oper_status = "unknown"

    for v in fvs:
	if v[0] == "oper_status":
	    oper_status = v[1]
	    break

    assert oper_status == "down"

def startup_link(dvs, db, port):
    dvs.servers[port].runcmd("ip link set up dev eth0") == 0

    time.sleep(1)

    tbl = swsscommon.Table(db, "PORT_TABLE")
    (status, fvs) = tbl.get("Ethernet%d" % (port * 4))

    assert status == True

    oper_status = "unknown"

    for v in fvs:
	if v[0] == "oper_status":
	    oper_status = v[1]
	    break

    assert oper_status == "up"

def run_warm_reboot(dvs):
    dvs.runcmd("config warm_restart enable swss")

    # Stop swss before modifing the configDB
    dvs.stop_swss()

    # start to apply new port_config.ini
    dvs.start_swss()
    dvs.runcmd(['sh', '-c', 'supervisorctl start neighsyncd'])
    dvs.runcmd(['sh', '-c', 'supervisorctl start restore_neighbors'])

    # Enabling some extra logging for validating the order of orchagent
    dvs.runcmd("swssloglevel -l INFO -c orchagent")

# Get state db route entry
def swss_get_route_entry_state(state_db,nh_memb_exp_count):
    stateroutetbl = swsscommon.Table(state_db, swsscommon.STATE_FG_ROUTE_TABLE_NAME)
    memb_dict = nh_memb_exp_count
    keys = stateroutetbl.getKeys()
    assert  len(keys) !=  0
    for key in keys:        
        (status, fvs) = stateroutetbl.get(key)
        assert status == True
        for fv in fvs:
            assert  fv[1] in nh_memb_exp_count
            memb_dict[fv[1]] = memb_dict[fv[1]] - 1 

    for idx,memb in memb_dict.items():
        assert memb == 0 


def validate_asic_nhg_regular_ecmp(asic_db, ipprefix):
    rtbl = swsscommon.Table(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
    nhgtbl = swsscommon.Table(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP")
    keys = rtbl.getKeys()

    found_route = False
    for k in keys:
        rt_key = json.loads(k)

        if rt_key['dest'] == ipprefix:
            found_route = True
            break
    
    # Now that ARP is populated, the route should be found
    assert found_route

    # assert the route points to next hop group
    (status, fvs) = rtbl.get(k)

    for v in fvs:
        if v[0] == "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID":
            nhgid = v[1]
            print nhgid
        elif v[0] == "SAI_NEXT_HOP_GROUP_ATTR_TYPE":
            nhg_type = v[1]

    assert nhgid is not None
    (status, fvs) = nhgtbl.get(nhgid)
    for v in fvs:
        if v[0] == "SAI_NEXT_HOP_GROUP_ATTR_TYPE":
            nhg_type = v[1] 

    assert nhg_type is not None
    assert nhg_type == "SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_UNORDERED_ECMP"

    return nhgid


def validate_asic_nhg_fg_ecmp(asic_db, ipprefix, size):
    rtbl = swsscommon.Table(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
    nhgtbl = swsscommon.Table(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP")
    keys = rtbl.getKeys()

    found_route = False
    for k in keys:
        rt_key = json.loads(k)

        if rt_key['dest'] == ipprefix:
            found_route = True
            break
    
    assert found_route

    # assert the route points to next hop group
    (status, fvs) = rtbl.get(k)

    nhg_type = ""
    nhg_cfg_size = 0

    for v in fvs:
        if v[0] == "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID":
            nhgid = v[1]
            print nhgid

    assert nhgid is not None
    (status, fvs) = nhgtbl.get(nhgid)

    for v in fvs:
        if v[0] == "SAI_NEXT_HOP_GROUP_ATTR_TYPE":
            nhg_type = v[1] 
        elif v[0] == "SAI_NEXT_HOP_GROUP_ATTR_CONFIGURED_SIZE":
            nhg_cfg_size = int(v[1])
    assert nhg_type is not None
    assert nhg_cfg_size is not None

    assert nhg_type == "SAI_NEXT_HOP_GROUP_TYPE_FINE_GRAIN_ECMP"
    assert nhg_cfg_size == size 

    return nhgid


def fine_grained_ecmp_base_test(dvs, match_mode):
    config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    fg_nhg_name = "fgnhg_v4"
    fg_nhg_prefix = "2.2.2.0/24"
    bucket_size = 60
    fvs_nul = [("NULL", "NULL")]
    NUM_NHs = 6
    ip_to_if_map = {}

    create_entry_tbl(
        config_db,
        "FG_NHG", '|', fg_nhg_name,
        [
            ("bucket_size", str(bucket_size)),
            ("match_mode", match_mode),
        ],
    )

    if match_mode == 'route-based':
        create_entry_tbl(
            config_db,
            "FG_NHG_PREFIX", '|', fg_nhg_prefix,
            [
                ("FG_NHG", fg_nhg_name),
            ],
        )

    for i in range(0,NUM_NHs):
        if_name_key = "Ethernet" + str(i*4)
        vlan_name_key = "Vlan" + str((i+1)*4)
        ip_pref_key = vlan_name_key + "|10.0.0." + str(i*2) + "/31"
        fvs = [("vlanid", str((i+1)*4))]
        create_entry_tbl(config_db, VLAN_TB , '|' , vlan_name_key, fvs)
        fvs = [("tagging_mode", "untagged")]
        create_entry_tbl(config_db, VLAN_MEMB_TB , '|' , vlan_name_key + "|" + if_name_key, fvs)
        create_entry_tbl(config_db, VLAN_IF_TB , '|' , vlan_name_key, fvs_nul)
        create_entry_tbl(config_db, VLAN_IF_TB , '|' , ip_pref_key, fvs_nul)
        dvs.runcmd("config interface startup " + if_name_key)
        dvs.servers[i].runcmd("ip link set down dev eth0") == 0
        dvs.servers[i].runcmd("ip link set up dev eth0") == 0
        bank = 0
        if i >= NUM_NHs/2:
            bank = 1
        fvs = [("FG_NHG", fg_nhg_name), ("bank", str(bank)), ("link", if_name_key)]
        create_entry_tbl(config_db, FG_NHG_MEMBER , '|' , "10.0.0." + str(1 + i*2), fvs)
        ip_to_if_map["10.0.0." + str(1 + i*2)] = vlan_name_key
    
    time.sleep(3)
    #"""

    db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
    state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)
    ps = swsscommon.ProducerStateTable(db, "ROUTE_TABLE")
    fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.7,10.0.0.9,10.0.0.11"), ("ifname","Vlan16,Vlan20,Vlan24")])

    ps.set(fg_nhg_prefix, fvs)

    time.sleep(1)

    # check if route was propagated to ASIC DB

    adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

    rtbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
    nhgtbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP")
    nhg_member_tbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER")
    nbtbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")

    keys = rtbl.getKeys()

    found_route = False
    for k in keys:
        rt_key = json.loads(k)

        if rt_key['dest'] == fg_nhg_prefix:
            found_route = True
            break

    # Since we didn't populate ARP yet, the route shouldn't be programmed
    assert (found_route == False)

    dvs.runcmd("arp -s 10.0.0.1 00:00:00:00:00:01")
    dvs.runcmd("arp -s 10.0.0.3 00:00:00:00:00:02")
    dvs.runcmd("arp -s 10.0.0.5 00:00:00:00:00:03")
    dvs.runcmd("arp -s 10.0.0.9 00:00:00:00:00:05")
    dvs.runcmd("arp -s 10.0.0.11 00:00:00:00:00:06")
    time.sleep(1)

    nhgid = validate_asic_nhg_fg_ecmp(adb, fg_nhg_prefix, bucket_size)

    (status, fvs) = nhgtbl.get(nhgid)
    assert status

    keys = nhg_member_tbl.getKeys()
    assert len(keys) == bucket_size

    # Obtain oids of NEXT_HOP asic entries
    nh_oid_map = {}

    for tbs in nbtbl.getKeys():
        (status, fvs) = nbtbl.get(tbs)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_NEXT_HOP_ATTR_IP":
                nh_oid_map[tbs] = fv[1]

    # Test addition of route with 0 members in bank
    # ARP is not resolved for 10.0.0.7, so fg nhg should be created with 10.0.0.7
    nh_memb_exp_count = {"10.0.0.9":30,"10.0.0.11":30}
    verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
    nh__exp_count = {"10.0.0.9@Vlan20":30,"10.0.0.11@Vlan24":30}
    swss_get_route_entry_state(state_db, nh__exp_count)

    dvs.runcmd("arp -s 10.0.0.7 00:00:00:00:00:04")
    time.sleep(1)

    for tbs in nbtbl.getKeys():
        (status, fvs) = nbtbl.get(tbs)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_NEXT_HOP_ATTR_IP":
                nh_oid_map[tbs] = fv[1]

    # Now that ARP was resolved, 10.0.0.7 should be added as a valid fg nhg member
    nh_memb_exp_count = {"10.0.0.7":20,"10.0.0.9":20,"10.0.0.11":20}
    verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
    nh__exp_count = {"10.0.0.7@Vlan16":20, "10.0.0.9@Vlan20":20,"10.0.0.11@Vlan24":20}
    swss_get_route_entry_state(state_db, nh__exp_count)

    # Bring down 1 next hop
    fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.7,10.0.0.11"), ("ifname", "Vlan16,Vlan24")])
    ps.set(fg_nhg_prefix, fvs)
    time.sleep(1)

    nh_memb_exp_count = {"10.0.0.7":30,"10.0.0.11":30}
    verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
    nh__exp_count = {"10.0.0.7@Vlan16":30, "10.0.0.11@Vlan24":30}
    swss_get_route_entry_state(state_db, nh__exp_count)

    # Bring up 1 next hop
    fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.7,10.0.0.9,10.0.0.11"), ("ifname", "Vlan16,Vlan20,Vlan24")])
    ps.set(fg_nhg_prefix, fvs)
    time.sleep(1)

    nh_memb_exp_count = {"10.0.0.7":20,"10.0.0.9":20,"10.0.0.11":20}
    verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
    nh__exp_count = {"10.0.0.7@Vlan16":20, "10.0.0.9@Vlan20":20,"10.0.0.11@Vlan24":20}
    swss_get_route_entry_state(state_db, nh__exp_count)

    run_warm_reboot(dvs)

    # assert the route points to next hop group
    nhgid = validate_asic_nhg_fg_ecmp(adb, fg_nhg_prefix, bucket_size)

    keys = nhg_member_tbl.getKeys()
    assert len(keys) == bucket_size

    # Obtain oids of NEXT_HOP asic entries
    nh_oid_map = {}

    for tbs in nbtbl.getKeys():
        (status, fvs) = nbtbl.get(tbs)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_NEXT_HOP_ATTR_IP":
                nh_oid_map[tbs] = fv[1]

    nh_memb_exp_count = {"10.0.0.7":20,"10.0.0.9":20,"10.0.0.11":20}
    verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
    nh__exp_count = {"10.0.0.7@Vlan16":20, "10.0.0.9@Vlan20":20,"10.0.0.11@Vlan24":20}
    swss_get_route_entry_state(state_db, nh__exp_count)


    # Bring up bank 0 next-hops in route for the 1st time
    fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.3,10.0.0.5,10.0.0.7,10.0.0.9,10.0.0.11"), ("ifname", "Vlan4,Vlan8,Vlan12,Vlan16,Vlan20,Vlan24")])
    ps.set(fg_nhg_prefix, fvs)
    time.sleep(1)

    nh_memb_exp_count = {"10.0.0.1":10,"10.0.0.3":10,"10.0.0.5":10,"10.0.0.7":10,"10.0.0.9":10,"10.0.0.11":10}
    verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
    nh__exp_count = {"10.0.0.1@Vlan4":10, "10.0.0.3@Vlan8":10,"10.0.0.5@Vlan12":10, "10.0.0.7@Vlan16":10, "10.0.0.9@Vlan20":10,"10.0.0.11@Vlan24":10}
    swss_get_route_entry_state(state_db, nh__exp_count)

    # Bring down arbitratry # of next-hops from both banks at the same time
    fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.5,10.0.0.11"), ("ifname", "Vlan4,Vlan12,Vlan24")])
    ps.set(fg_nhg_prefix, fvs)
    time.sleep(1)

    nh_memb_exp_count = {"10.0.0.1":15,"10.0.0.5":15,"10.0.0.11":30}
    verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
    nh__exp_count = {"10.0.0.1@Vlan4":15, "10.0.0.5@Vlan12":15, "10.0.0.11@Vlan24":30}
    swss_get_route_entry_state(state_db, nh__exp_count)

    # Bring down 1 member and bring up 1 member in bank 0 at the same time
    fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.3,10.0.0.11"), ("ifname", "Vlan4,Vlan8,Vlan24")])
    ps.set(fg_nhg_prefix, fvs)
    time.sleep(1)

    nh_memb_exp_count = {"10.0.0.1":15,"10.0.0.3":15,"10.0.0.11":30}
    verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
    nh__exp_count = {"10.0.0.1@Vlan4":15, "10.0.0.3@Vlan8":15, "10.0.0.11@Vlan24":30}
    swss_get_route_entry_state(state_db, nh__exp_count)

    # Bring down 2 members and bring up 1 member in bank 0 at the same time
    fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.5,10.0.0.11"), ("ifname", "Vlan12,Vlan24")])
    ps.set(fg_nhg_prefix, fvs)
    time.sleep(1)

    nh_memb_exp_count = {"10.0.0.5":30,"10.0.0.11":30}
    verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
    nh__exp_count = {"10.0.0.5@Vlan12":30, "10.0.0.11@Vlan24":30}
    swss_get_route_entry_state(state_db, nh__exp_count)

    # Bring up 2 members and bring down 1 member in bank 0 at the same time
    fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.3,10.0.0.11"), ("ifname", "Vlan4,Vlan8,Vlan24")])
    ps.set(fg_nhg_prefix, fvs)
    time.sleep(1)

    nh_memb_exp_count = {"10.0.0.1":15,"10.0.0.3":15,"10.0.0.11":30}
    verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
    nh__exp_count = {"10.0.0.1@Vlan4":15,"10.0.0.3@Vlan8":15,"10.0.0.11@Vlan24":30}
    swss_get_route_entry_state(state_db, nh__exp_count)

    # Bringup arbitrary # of next-hops from both banks at the same time
    fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.3,10.0.0.5,10.0.0.7,10.0.0.9,10.0.0.11"), ("ifname", "Vlan4,Vlan8,Vlan12,Vlan16,Vlan20,Vlan24")])
    ps.set(fg_nhg_prefix, fvs)
    time.sleep(1)

    nh_memb_exp_count = {"10.0.0.1":10,"10.0.0.3":10,"10.0.0.5":10,"10.0.0.7":10,"10.0.0.9":10,"10.0.0.11":10}
    verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
    nh__exp_count = {"10.0.0.1@Vlan4":10, "10.0.0.3@Vlan8":10,"10.0.0.5@Vlan12":10, "10.0.0.7@Vlan16":10, "10.0.0.9@Vlan20":10,"10.0.0.11@Vlan24":10}
    swss_get_route_entry_state(state_db, nh__exp_count)

    print "########################test warm reboot#########################################"
    #############################################################################
    #                                                                           #
    #                        swss Warm-Restart Testing Begin                    #
    #                                                                           #
    #############################################################################
    run_warm_reboot(dvs)

    # assert the route points to next hop group
    nhgid = validate_asic_nhg_fg_ecmp(adb, fg_nhg_prefix, bucket_size)

    keys = nhg_member_tbl.getKeys()
    assert len(keys) == bucket_size

    # Obtain oids of NEXT_HOP asic entries
    nh_oid_map = {}

    for tbs in nbtbl.getKeys():
        (status, fvs) = nbtbl.get(tbs)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_NEXT_HOP_ATTR_IP":
                nh_oid_map[tbs] = fv[1]

    nh_memb_exp_count = {"10.0.0.1":10,"10.0.0.3":10,"10.0.0.5":10,"10.0.0.7":10,"10.0.0.9":10,"10.0.0.11":10}
    verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
    nh__exp_count = {"10.0.0.1@Vlan4":10, "10.0.0.3@Vlan8":10,"10.0.0.5@Vlan12":10, "10.0.0.7@Vlan16":10, "10.0.0.9@Vlan20":10,"10.0.0.11@Vlan24":10}
    swss_get_route_entry_state(state_db, nh__exp_count)

    print "########################Recover from warm reboot#########################################"

    #############################################################################
    #                                                                           #
    #                        swss Warm-Restart Testing END                      #
    #                            get to normal state                            #
    #############################################################################

    # Test bank down
    fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.3,10.0.0.5"), ("ifname", "Vlan4,Vlan8,Vlan12")])
    ps.set(fg_nhg_prefix, fvs)
    time.sleep(1)

    nh_memb_exp_count = {"10.0.0.1":20,"10.0.0.3":20,"10.0.0.5":20}
    verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
    nh__exp_count = {"10.0.0.1@Vlan4":20, "10.0.0.3@Vlan8":20,"10.0.0.5@Vlan12":20}
    swss_get_route_entry_state(state_db, nh__exp_count)

    # Test bank down: nh change in active bank
    fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.5"), ("ifname", "Vlan4,Vlan12")])
    ps.set(fg_nhg_prefix, fvs)
    time.sleep(1)

    nh_memb_exp_count = {"10.0.0.1":30,"10.0.0.5":30}
    verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
    nh__exp_count = {"10.0.0.1@Vlan4":30, "10.0.0.5@Vlan12":30}
    swss_get_route_entry_state(state_db, nh__exp_count)


    # Test 1st memb up in bank
    fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.5,10.0.0.11"), ("ifname", "Vlan4,Vlan12,Vlan24")])
    ps.set(fg_nhg_prefix, fvs)
    time.sleep(1)

    nh_memb_exp_count = {"10.0.0.1":15,"10.0.0.5":15,"10.0.0.11":30}
    verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
    nh__exp_count = {"10.0.0.1@Vlan4":15, "10.0.0.5@Vlan12":15, "10.0.0.11@Vlan24":30}
    swss_get_route_entry_state(state_db, nh__exp_count)

    # Test 2nd,3rd memb up in bank
    fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.5,10.0.0.7,10.0.0.9,10.0.0.11"), ("ifname", "Vlan4,Vlan12,Vlan16,Vlan20,Vlan24")])
    ps.set(fg_nhg_prefix, fvs)
    time.sleep(1)

    nh_memb_exp_count = {"10.0.0.1":15,"10.0.0.5":15,"10.0.0.7":10,"10.0.0.9":10,"10.0.0.11":10}
    verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
    nh__exp_count = {"10.0.0.1@Vlan4":15,"10.0.0.5@Vlan12":15, "10.0.0.7@Vlan16":10, "10.0.0.9@Vlan20":10,"10.0.0.11@Vlan24":10}
    swss_get_route_entry_state(state_db, nh__exp_count)

    # bring all links down one by one
    shutdown_link(dvs, db, 0)	
    nh_memb_exp_count = {"10.0.0.5":30,"10.0.0.7":10,"10.0.0.9":10,"10.0.0.11":10}
    verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
    nh__exp_count = {"10.0.0.5@Vlan12":30, "10.0.0.7@Vlan16":10, "10.0.0.9@Vlan20":10,"10.0.0.11@Vlan24":10}
    swss_get_route_entry_state(state_db, nh__exp_count)

    shutdown_link(dvs, db, 2)
    nh_memb_exp_count = {"10.0.0.7":20,"10.0.0.9":20,"10.0.0.11":20}
    verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
    nh__exp_count = {"10.0.0.7@Vlan16":20, "10.0.0.9@Vlan20":20,"10.0.0.11@Vlan24":20}
    swss_get_route_entry_state(state_db, nh__exp_count)

    shutdown_link(dvs, db, 3)
    shutdown_link(dvs, db, 4)
    nh_memb_exp_count = {"10.0.0.11":60}
    verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
    nh__exp_count = {"10.0.0.11@Vlan24":60}
    swss_get_route_entry_state(state_db, nh__exp_count)

    # Bring down last link, there shouldn't be a crash or other bad orchagent state because of this
    shutdown_link(dvs, db, 5)

    # bring all links up one by one
    startup_link(dvs, db, 3)
    startup_link(dvs, db, 4)
    startup_link(dvs, db, 5)
    nh_memb_exp_count = {"10.0.0.7":20,"10.0.0.9":20,"10.0.0.11":20}
    verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
    nh__exp_count = {"10.0.0.7@Vlan16":20, "10.0.0.9@Vlan20":20,"10.0.0.11@Vlan24":20}
    swss_get_route_entry_state(state_db, nh__exp_count)

    startup_link(dvs, db, 2)
    nh_memb_exp_count = {"10.0.0.5":30,"10.0.0.7":10,"10.0.0.9":10,"10.0.0.11":10}
    verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
    nh__exp_count = {"10.0.0.5@Vlan12":30,"10.0.0.7@Vlan16":10, "10.0.0.9@Vlan20":10,"10.0.0.11@Vlan24":10}
    swss_get_route_entry_state(state_db, nh__exp_count)

    startup_link(dvs, db, 0)
    nh_memb_exp_count = {"10.0.0.1":15,"10.0.0.5":15,"10.0.0.7":10,"10.0.0.9":10,"10.0.0.11":10}
    verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
    nh__exp_count = {"10.0.0.1@Vlan4":15,"10.0.0.5@Vlan12":15,"10.0.0.7@Vlan16":10, "10.0.0.9@Vlan20":10,"10.0.0.11@Vlan24":10}
    swss_get_route_entry_state(state_db, nh__exp_count)

    # remove fgnhg member
    remove_entry_tbl(
        config_db,
        "FG_NHG_MEMBER", 
        "10.0.0.1",
    )
    nh_memb_exp_count = {"10.0.0.5":30,"10.0.0.7":10,"10.0.0.9":10,"10.0.0.11":10}
    verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
    nh__exp_count = {"10.0.0.5@Vlan12":30,"10.0.0.7@Vlan16":10, "10.0.0.9@Vlan20":10,"10.0.0.11@Vlan24":10}
    swss_get_route_entry_state(state_db, nh__exp_count)

    # add fgnhg member
    create_entry_tbl(
        config_db,
        "FG_NHG_MEMBER", '|', "10.0.0.1",
        [
            ("FG_NHG", fg_nhg_name),
            ("bank", "0"),
        ],
    )
    nh_memb_exp_count = {"10.0.0.1":15,"10.0.0.5":15,"10.0.0.7":10,"10.0.0.9":10,"10.0.0.11":10}
    verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
    nh__exp_count = {"10.0.0.1@Vlan4":15,"10.0.0.5@Vlan12":15,"10.0.0.7@Vlan16":10, "10.0.0.9@Vlan20":10,"10.0.0.11@Vlan24":10}
    swss_get_route_entry_state(state_db, nh__exp_count)

    # Remove route
    ps._del(fg_nhg_prefix)
    time.sleep(1)

    keys = rtbl.getKeys()
    for k in keys:
        rt_key = json.loads(k)

        assert rt_key['dest'] != fg_nhg_prefix

    keys = nhg_member_tbl.getKeys()
    assert len(keys) == 0
    
    stateroute_tbl = swsscommon.Table(state_db, swsscommon.STATE_FG_ROUTE_TABLE_NAME)
    keys = stateroute_tbl.getKeys()
    assert len(keys) == 0
    
    if match_mode == 'route-based':
        remove_entry_tbl(
            config_db,
            "FG_NHG_PREFIX", 
            fg_nhg_prefix,
        )

        # add normal route
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.7,10.0.0.9,10.0.0.11"), ("ifname", "Vlan16,Vlan20,Vlan24")])
        ps.set(fg_nhg_prefix, fvs)

        time.sleep(1)

        nhgid = validate_asic_nhg_regular_ecmp(adb, fg_nhg_prefix)

        keys = nhg_member_tbl.getKeys()
        assert len(keys) == 3

        # add fgnhg prefix
        create_entry_tbl(
            config_db,
            "FG_NHG_PREFIX", '|', fg_nhg_prefix,
            [
                ("FG_NHG", fg_nhg_name),
            ],
        )

        nhgid = validate_asic_nhg_fg_ecmp(adb, fg_nhg_prefix, bucket_size)
        
        keys = nhg_member_tbl.getKeys()
        assert len(keys) == bucket_size

        # Obtain oids of NEXT_HOP asic entries
        nh_oid_map = {}

        for tbs in nbtbl.getKeys():
            (status, fvs) = nbtbl.get(tbs)
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_NEXT_HOP_ATTR_IP":
                    nh_oid_map[tbs] = fv[1]

        nh_memb_exp_count = {"10.0.0.7":20,"10.0.0.9":20,"10.0.0.11":20}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
        nh__exp_count = {"10.0.0.7@Vlan16":20, "10.0.0.9@Vlan20":20,"10.0.0.11@Vlan24":20}
        swss_get_route_entry_state(state_db, nh__exp_count)

        # remove fgnhg prefix
        remove_entry_tbl(
            config_db,
            "FG_NHG_PREFIX", 
            fg_nhg_prefix,
        )

        time.sleep(1)


        nhgid = validate_asic_nhg_regular_ecmp(adb, fg_nhg_prefix)

        keys = nhg_member_tbl.getKeys()
        assert len(keys) == 3

        # remove prefix entry
        ps._del(fg_nhg_prefix)
        time.sleep(1)

        keys = rtbl.getKeys()
        for k in keys:
            rt_key = json.loads(k)

            assert rt_key['dest'] != fg_nhg_prefix

        keys = nhg_member_tbl.getKeys()
        assert len(keys) == 0

    # remove group fail since there's still nexthop member
    remove_entry_tbl(
        config_db,
        "FG_NHG", 
        fg_nhg_name,
    )

    for i in range(0,NUM_NHs):
        if_name_key = "Ethernet" + str(i*4)
        vlan_name_key = "Vlan" + str((i+1)*4)
        ip_pref_key = vlan_name_key + "|10.0.0." + str(i*2) + "/31"
        remove_entry_tbl(config_db, VLAN_MEMB_TB , vlan_name_key + "|" + if_name_key)
        remove_entry_tbl(config_db, VLAN_IF_TB , vlan_name_key)
        remove_entry_tbl(config_db, VLAN_IF_TB , ip_pref_key)
        remove_entry_tbl(config_db, VLAN_TB , vlan_name_key)
        dvs.runcmd("config interface shutdown " + if_name_key)
        dvs.servers[i].runcmd("ip link set down dev eth0") == 0
        remove_entry_tbl(config_db, FG_NHG_MEMBER , "10.0.0." + str(1 + i*2))

    # remove group should succeeds
    remove_entry_tbl(
        config_db,
        "FG_NHG", 
        fg_nhg_name,
    )


class TestFineGrainedNextHopGroup(object):
    def test_fgnhg_matchmode_route(self, dvs, testlog):
        '''
        Test for match_mode route-based
        '''
        fine_grained_ecmp_base_test(dvs, 'route-based')

    def test_fgnhg_matchmode_nexthop(self, dvs, testlog):
        '''
        Test for match_mode nexthop-based
        '''
        fine_grained_ecmp_base_test(dvs, 'nexthop-based')

    def test_fgnhg_more_nhs_nondiv_bucket_size(self, dvs, testlog):
        '''
        Test for Fine Grained next hop groups with more nexthops.
        Set up the test such that: there is a larger bucket size and the number of buckets 
        are not equally divisible amongst nexthops. Different physical interface type for dynamicitiy.
        '''
        config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        fg_nhg_name = "new_fgnhg_v4"
        fg_nhg_prefix = "3.3.3.0/24"
        # Test with non-divisible bucket size
        bucket_size = 128
        NUM_NHs = 10
        fvs_nul = [("NULL", "NULL")]

        create_entry_tbl(
            config_db,
            "FG_NHG", '|', fg_nhg_name,
            [
                ("bucket_size", str(bucket_size)),
            ],
        )

        create_entry_tbl(
            config_db,
            "FG_NHG_PREFIX", '|', fg_nhg_prefix,
            [
                ("FG_NHG", fg_nhg_name),
            ],
        )
        for i in range(0,NUM_NHs):
            if_name_key = "Ethernet" + str(i*4)
            ip_pref_key = if_name_key + "|10.0.0." + str(i*2) + "/31"
            create_entry_tbl(config_db, IF_TB , '|' , if_name_key, fvs_nul)
            create_entry_tbl(config_db, IF_TB , '|' , ip_pref_key, fvs_nul)
            dvs.runcmd("config interface startup " + if_name_key)
            dvs.servers[i].runcmd("ip link set down dev eth0") == 0
            dvs.servers[i].runcmd("ip link set up dev eth0") == 0
            bank = 0
            if i >= NUM_NHs/2:
                bank = 1
            fvs = [("FG_NHG", fg_nhg_name), ("bank", str(bank)), ("link", if_name_key)]
            create_entry_tbl(config_db, FG_NHG_MEMBER , '|' , "10.0.0." + str(1 + i*2), fvs)
            dvs.runcmd("arp -s 10.0.0." + str(1 + i*2) + " 00:00:00:00:00:" + str(1 + i*2))
        
        time.sleep(3)


        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        ps = swsscommon.ProducerStateTable(db, "ROUTE_TABLE")
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.11"),
            ("ifname", "Ethernet0,Ethernet20")])

        ps.set(fg_nhg_prefix, fvs)

        time.sleep(1)

        # check if route was propagated to ASIC DB

        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        rtbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        nhgtbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP")
        nhg_member_tbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER")
        nbtbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")

        keys = rtbl.getKeys()

        found_route = False
        for k in keys:
            rt_key = json.loads(k)

            if rt_key['dest'] == fg_nhg_prefix:
                found_route = True
                break

        assert found_route
        # assert the route points to next hop group
        (status, fvs) = rtbl.get(k)

        for v in fvs:
            if v[0] == "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID":
                nhgid = v[1]

        (status, fvs) = nhgtbl.get(nhgid)
        assert status

        keys = nhg_member_tbl.getKeys()
        assert len(keys) == bucket_size

        # Obtain oids of NEXT_HOP asic entries
        nh_oid_map = {}

        for tbs in nbtbl.getKeys():
            (status, fvs) = nbtbl.get(tbs)
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_NEXT_HOP_ATTR_IP":
                    nh_oid_map[tbs] = fv[1]

        # Test addition of route with 0 members in bank
        nh_memb_exp_count = {"10.0.0.1":64,"10.0.0.11":64}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)

        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.3,10.0.0.5,10.0.0.11,10.0.0.13,10.0.0.15"),
            ("ifname", "Ethernet0,Ethernet4,Ethernet8,Ethernet20,Ethernet24,Ethernet28")])
        ps.set(fg_nhg_prefix, fvs)
        time.sleep(1)
        nh_memb_exp_count = {"10.0.0.1":22,"10.0.0.3":21,"10.0.0.5":21,"10.0.0.11":22,"10.0.0.13":21,"10.0.0.15":21}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)


        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.3,10.0.0.5,10.0.0.7,10.0.0.9,10.0.0.11,10.0.0.13,10.0.0.15,10.0.0.17,10.0.0.19"),
            ("ifname", "Ethernet0,Ethernet4,Ethernet8,Ethernet12,Ethernet16,Ethernet20,Ethernet24,Ethernet28,Ethernet32,Ethernet36")])
        ps.set(fg_nhg_prefix, fvs)
        time.sleep(1)
        nh_memb_exp_count = {"10.0.0.1":13,"10.0.0.3":13,"10.0.0.5":13,"10.0.0.7":12,"10.0.0.9":13,"10.0.0.11":13,"10.0.0.13":13,"10.0.0.15":13,"10.0.0.17":12,"10.0.0.19":13}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)


        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.3,10.0.0.5,10.0.0.7,10.0.0.9,10.0.0.11,10.0.0.13,10.0.0.19"),
            ("ifname", "Ethernet4,Ethernet8,Ethernet12,Ethernet16,Ethernet20,Ethernet24,Ethernet36")])
        ps.set(fg_nhg_prefix, fvs)
        time.sleep(1)
        nh_memb_exp_count = {"10.0.0.3":16,"10.0.0.5":16,"10.0.0.7":16,"10.0.0.9":16,"10.0.0.11":22,"10.0.0.13":21,"10.0.0.19":21}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)


        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.3,10.0.0.7,10.0.0.9,10.0.0.13,10.0.0.15,10.0.0.17,10.0.0.19"),
            ("ifname", "Ethernet4,Ethernet12,Ethernet16,Ethernet24,Ethernet28,Ethernet32,Ethernet36")])
        ps.set(fg_nhg_prefix, fvs)
        time.sleep(1)
        nh_memb_exp_count = {"10.0.0.3":22,"10.0.0.7":21,"10.0.0.9":21,"10.0.0.13":16,"10.0.0.15":16,"10.0.0.17":16,"10.0.0.19":16}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)


        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.7,10.0.0.11"), ("ifname", "Ethernet12,Ethernet20")])
        ps.set(fg_nhg_prefix, fvs)
        time.sleep(1)
        nh_memb_exp_count = {"10.0.0.7":64,"10.0.0.11":64}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)


        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.5,10.0.0.7,10.0.0.9"), ("ifname", "Ethernet8,Ethernet12,Ethernet16")])
        ps.set(fg_nhg_prefix, fvs)
        time.sleep(1)
        nh_memb_exp_count = {"10.0.0.5":43,"10.0.0.7":43,"10.0.0.9":42}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)

        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.3,10.0.0.5,10.0.0.7,10.0.0.9,10.0.0.11"), ("ifname", "Ethernet0,Ethernet4,Ethernet8,Ethernet12,Ethernet16,Ethernet20")])
        ps.set(fg_nhg_prefix, fvs)
        time.sleep(1)
        nh_memb_exp_count = {"10.0.0.1":12,"10.0.0.3":13,"10.0.0.5":13,"10.0.0.7":13,"10.0.0.9":13,"10.0.0.11":64}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)


        # Remove route
        ps._del(fg_nhg_prefix)
        time.sleep(1)

        keys = rtbl.getKeys()
        for k in keys:
            rt_key = json.loads(k)

            assert rt_key['dest'] != fg_nhg_prefix

        keys = nhg_member_tbl.getKeys()
        assert len(keys) == 0

        for i in range(0,NUM_NHs):
            if_name_key = "Ethernet" + str(i*4)
            ip_pref_key = if_name_key + "|10.0.0." + str(i*2) + "/31"
            remove_entry_tbl(config_db, IF_TB,  ip_pref_key)
            remove_entry_tbl(config_db, IF_TB,  if_name_key)
            dvs.runcmd("config interface shutdown " + if_name_key)
            dvs.servers[i].runcmd("ip link set down dev eth0") == 0
            remove_entry_tbl(config_db, FG_NHG_MEMBER, "10.0.0." + str(1 + i*2))
        
        # remove fgnhg prefix
        remove_entry_tbl(
            config_db,
            "FG_NHG_PREFIX",
            fg_nhg_prefix,
        )

        remove_entry_tbl(
            config_db,
            "FG_NHG", 
            fg_nhg_name,
        )


    def test_fgnhg_matchmode_nexthop_multi_route(self, dvs, testlog):
        '''
        Test route/nh transitions to/from Fine Grained ECMP and Regular ECMP.
        Create multiple prefixes pointing to the Fine Grained nhs and ensure 
        fine grained ECMP ASIC objects were created for this scenario as expected.
        '''
        fg_nhg_name = "fgnhg_v4"
        fg_nhg_prefix = "3.3.3.0/24"
        # Test with non-divisible bucket size
        bucket_size = 128
        NUM_NHs = 4
        NUM_NHs_non_fgnhg = 2
        fvs_nul = [("NULL", "NULL")]
        nh_oid_map = {}

        config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        # Initialize base config
        # Test with non-divisible bucket size

        create_entry_tbl(
            config_db,
            "FG_NHG", '|', fg_nhg_name,
            [
                ("bucket_size", str(bucket_size)),
                ("match_mode", "nexthop-based")
            ],
        )

        # Create 2 more interface + IPs for non-fine grained ECMP validation
        for i in range(0,NUM_NHs):
            if_name_key = "Ethernet" + str(i*4)
            ip_pref_key = if_name_key + "|10.0.0." + str(i*2) + "/31"
            create_entry_tbl(config_db, IF_TB , '|' , if_name_key, fvs_nul)
            create_entry_tbl(config_db, IF_TB , '|' , ip_pref_key, fvs_nul)
            dvs.runcmd("config interface startup " + if_name_key)
            dvs.servers[i].runcmd("ip link set down dev eth0") == 0
            dvs.servers[i].runcmd("ip link set up dev eth0") == 0
            bank = 0
            if i >= NUM_NHs/2:
                bank = 1
            fvs = [("FG_NHG", fg_nhg_name), ("bank", str(bank)), ("link", if_name_key)]
            create_entry_tbl(config_db, FG_NHG_MEMBER , '|' , "10.0.0." + str(1 + i*2), fvs)
            dvs.runcmd("arp -s 10.0.0." + str(1 + i*2) + " 00:00:00:00:00:" + str(1 + i*2))
        
        for i in range(NUM_NHs, NUM_NHs + NUM_NHs_non_fgnhg):
            if_name_key = "Ethernet" + str(i*4)
            ip_pref_key = if_name_key + "|10.0.0." + str(i*2) + "/31"
            create_entry_tbl(config_db, IF_TB , '|' , if_name_key, fvs_nul)
            create_entry_tbl(config_db, IF_TB , '|' , ip_pref_key, fvs_nul)
            dvs.runcmd("config interface startup " + if_name_key)
            dvs.servers[i].runcmd("ip link set down dev eth0") == 0
            dvs.servers[i].runcmd("ip link set up dev eth0") == 0
            dvs.runcmd("arp -s 10.0.0." + str(1 + i*2) + " 00:00:00:00:00:" + str(1 + i*2))

        time.sleep(3)

        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)

        rtbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        nhgtbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP")
        nhg_member_tbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER")
        nbtbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")

        # Program the route
        ps = swsscommon.ProducerStateTable(db, "ROUTE_TABLE")
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.5"),
            ("ifname", "Ethernet0,Ethernet8")])
        ps.set(fg_nhg_prefix, fvs)
        time.sleep(1)

        # Validate that the correct ASIC DB elements were setup per Fine Grained ECMP
        nhgid = validate_asic_nhg_fg_ecmp(adb, fg_nhg_prefix, bucket_size)        

        nh_oid_map = {}

        for tbs in nbtbl.getKeys():
            (status, fvs) = nbtbl.get(tbs)
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_NEXT_HOP_ATTR_IP":
                    nh_oid_map[tbs] = fv[1]

        keys = nhg_member_tbl.getKeys()
        assert len(keys) == bucket_size

        # The route had been created with 0 members in bank
        nh_memb_exp_count = {"10.0.0.1":64,"10.0.0.5":64}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)

        # Add a 2nd prefix associated with the same set of next-hops
        fg_nhg_prefix_2 = "5.5.5.0/16"
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.5"),
            ("ifname", "Ethernet0,Ethernet8")])
        ps.set(fg_nhg_prefix_2, fvs)
        time.sleep(1)

        keys = nhg_member_tbl.getKeys()
        assert len(keys) == bucket_size*2

        nhgid_2 = validate_asic_nhg_fg_ecmp(adb, fg_nhg_prefix_2, bucket_size)
        nh_memb_exp_count = {"10.0.0.1":64,"10.0.0.5":64}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid_2,bucket_size)

        # Add a 3rd prefix with a next-hop(10.0.0.9) not defined for FG ECMP
        # Should end up as regular ECMP
        fg_nhg_prefix_3 = "6.6.6.0/16"
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.5,10.0.0.9"),
                ("ifname", "Ethernet0,Ethernet8,Ethernet16")])
        ps.set(fg_nhg_prefix_3, fvs)
        time.sleep(1)
        validate_asic_nhg_regular_ecmp(adb, fg_nhg_prefix_3)
        keys = nhg_member_tbl.getKeys()
        assert len(keys) == bucket_size*2 + 3

        # Remove the 10.0.0.9 next-hop, it should now transition to Fine Grained ECMP
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.5"),
            ("ifname", "Ethernet0,Ethernet8")])
        ps.set(fg_nhg_prefix_3, fvs)
        time.sleep(1)
        nhgid_3 = validate_asic_nhg_fg_ecmp(adb, fg_nhg_prefix_3, bucket_size)
        keys = nhg_member_tbl.getKeys()
        assert len(keys) == bucket_size*3
        nh_memb_exp_count = {"10.0.0.1":64,"10.0.0.5":64}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid_3,bucket_size)

        # Add the 10.0.0.9 next-hop again, it should transition back to regular ECMP
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.5,10.0.0.9"),
                ("ifname", "Ethernet0,Ethernet8,Ethernet16")])
        ps.set(fg_nhg_prefix_3, fvs)
        time.sleep(1)
        validate_asic_nhg_regular_ecmp(adb, fg_nhg_prefix_3)
        keys = nhg_member_tbl.getKeys()
        assert len(keys) == bucket_size*2 + 3

        # Delete the prefix
        ps._del(fg_nhg_prefix_3)
        time.sleep(1)
        found_route = False
        keys = rtbl.getKeys()
        for k in keys:
            rt_key = json.loads(k)
            if rt_key['dest'] == fg_nhg_prefix_3:
                found_route = True
                break
        assert found_route == False
        keys = nhg_member_tbl.getKeys()
        assert len(keys) == bucket_size*2
        keys = nhgtbl.getKeys()
        assert len(keys) == 2

        # Change FG nhs for one route, ensure that the other route nh is unaffected
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.3,10.0.0.5,10.0.0.7"),
            ("ifname", "Ethernet0,Ethernet4,Ethernet8,Ethernet12")])
        ps.set(fg_nhg_prefix, fvs)
        time.sleep(1)
        nh_memb_exp_count = {"10.0.0.1":32,"10.0.0.3":32,"10.0.0.5":32,"10.0.0.7":32}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
        nh_memb_exp_count = {"10.0.0.1":64,"10.0.0.5":64}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid_2,bucket_size)

        # Remove route
        # remove prefix entry
        ps._del(fg_nhg_prefix)
        time.sleep(1)
        found_route = False
        keys = rtbl.getKeys()
        for k in keys:
            rt_key = json.loads(k)
            if rt_key['dest'] == fg_nhg_prefix:
                found_route = True
                break
        assert found_route == False
        keys = nhg_member_tbl.getKeys()
        assert len(keys) == bucket_size
        keys = nhgtbl.getKeys()
        assert len(keys) == 1
        # Ensure that 2nd route is still here and then delete it
        nh_memb_exp_count = {"10.0.0.1":64,"10.0.0.5":64}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid_2,bucket_size)
        # Delete the 2nd route as well
        ps._del(fg_nhg_prefix_2)
        time.sleep(1)
        found_route = False
        keys = rtbl.getKeys()
        for k in keys:
            rt_key = json.loads(k)
            if rt_key['dest'] == fg_nhg_prefix_2:
                found_route = True
                break
        assert found_route == False
        keys = nhg_member_tbl.getKeys()
        assert len(keys) == 0
        keys = nhgtbl.getKeys()
        assert len(keys) == 0

        # cleanup all entries
        for i in range(0,NUM_NHs + NUM_NHs_non_fgnhg):
            if_name_key = "Ethernet" + str(i*4)
            ip_pref_key = if_name_key + "|10.0.0." + str(i*2) + "/31"
            remove_entry_tbl(config_db, IF_TB,  ip_pref_key)
            remove_entry_tbl(config_db, IF_TB,  if_name_key)
            dvs.runcmd("config interface shutdown " + if_name_key)
            dvs.servers[i].runcmd("ip link set down dev eth0") == 0
            remove_entry_tbl(config_db, FG_NHG_MEMBER, "10.0.0." + str(1 + i*2))

        remove_entry_tbl(
            config_db,
            "FG_NHG", 
            fg_nhg_name,
        )
