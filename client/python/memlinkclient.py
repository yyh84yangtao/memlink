#coding: utf-8
import os, sys
from memlink import *

class MemLinkClietException (Exception):
    pass

class MemLinkClient:
    def __init__(self, host, readport, writeport, timeout):
        self.client = memlink_create(host, readport, writeport, timeout)
        if not self.client:
            raise MemLinkClietException

    def close(self):
        if self.client:
            memlink_close(self.client)

    def destroy(self):
        if self.client:
            memlink_destroy(self.client)
            self.client = None

    def ping(self):
        return memlink_cmd_ping(self.client)

    def dump(self):
        return memlink_cmd_dump(self.client)

    def clean(self, key):
        return memlink_cmd_clean(self.client, key)

    def create(self, key, valuesize, maskstr, listtype, valuetype):
        return memlink_cmd_create(self.client, key, valuesize, maskstr, listtype, valuetype)

    def create_list(self, key, valuesize, maskstr):
        return memlink_cmd_create_list(self.client, key, valuesize, maskstr);
    
    def create_queue(self, key, valuesize, maskstr):
        return memlink_cmd_create_queue(self.client, key, valuesize, maskstr);

    def create_sortlist(self, key, valuesize, maskstr, valuetype):
        return memlink_cmd_create_sortlist(self.client, key, valuesize, maskstr, valuetype);

    def stat(self, key):
        mstat = MemLinkStat()
        ret = memlink_cmd_stat(self.client, key, mstat)
        if ret != MEMLINK_OK:
            mstat = None
        return ret, mstat

    def stat_sys(self):
        mstat = MemLinkStatSys()
        ret = memlink_cmd_stat_sys(self.client, mstat)
        if ret != MEMLINK_OK:
            mstat = None
        return ret, mstat

    def delete(self, key, value):
        return memlink_cmd_del(self.client, key, value, len(value))

    def delete_by_mask(self, key, mask):
        return memlink_cmd_del_by_mask(self.client, key, mask)

    def insert(self, key, value, maskstr, pos):
        return memlink_cmd_insert(self.client, key, value, len(value), maskstr, pos)

    def sortlist_insert(self, key, value, maskstr):
        return memlink_cmd_insert(self.client, key, value, len(value), maskstr, -1)


    #def insert_mvalue(self, key, items):
    #    return memlink_cmd_insert_mvalue(self.client, key, values, num)

    def move(self, key, value, pos):
        return memlink_cmd_move(self.client, key, value, len(value), pos)

    def mask(self, key, value, maskstr):
        return memlink_cmd_mask(self.client, key, value, len(value), maskstr)

    def tag(self, key, value, tag):
        return memlink_cmd_tag(self.client, key, value, len(value), tag)

    def range(self, key, kind, maskstr, frompos, rlen):
        result = MemLinkResult()
        ret = memlink_cmd_range(self.client, key, kind, maskstr, frompos, rlen, result)
        if ret != MEMLINK_OK:
            result = None
        return ret, result

    def rmkey(self, key):
        return memlink_cmd_rmkey(self.client, key)

    def count(self, key, maskstr):
        result = MemLinkCount()
        ret = memlink_cmd_count(self.client, key, maskstr, result)
        if ret != MEMLINK_OK:
            result = None
        return ret, result

    def lpush(self, key, value, maskstr):
        return memlink_cmd_lpush(self.client, key, value, len(value), maskstr)

    def rpush(self, key, value, maskstr):
        return memlink_cmd_rpush(self.client, key, value, len(value), maskstr)

    def lpop(self, key, num=1):
        result = MemLinkResult()
        ret = memlink_cmd_lpop(self.client, key, num, result)
        if ret != MEMLINK_OK:
            result = None
        return ret, result

    def rpop(self, key, num=1):
        result = MemLinkResult()
        ret = memlink_cmd_rpop(self.client, key, num, result)
        if ret != MEMLINK_OK:
            result = None
        return ret, result




def memlinkresult_free(self):
    memlink_result_free(self)

def memlinkresult_print(self):
    s = 'count:%d valuesize:%d masksize:%d\n' % (self.count, self.valuesize, self.masksize)
    item = self.root
    while item:
        s += 'value:%s mask:%s\n' % (item.value, repr(item.mask))
        item = item.next
    return s


MemLinkResult.close   = memlinkresult_free
MemLinkResult.__del__ = memlinkresult_free
MemLinkResult.__str__ = memlinkresult_print

def memlinkstat_print(self):
    s = 'valuesize:%d\nmasksize:%d\nblocks:%d\ndata:%d\ndata_used:%d\nmem:%d\n' % \
        (self.valuesize, self.masksize, self.blocks, self.data, self.data_used, self.mem)

    return s

MemLinkStat.__str__ = memlinkstat_print

def memlinkstatsys_print(self):
    s = 'keys:%d\nvalues:%d\nblocks:%d\ndata:%d\ndata_used:%d\nblock_values:%d\nht_mem:%d\npool_blocks:%d\n' % \
        (self.keys, self.values, self.blocks, self.data, self.data_used, self.block_values, self.ht_mem, self.pool_blocks)

    return s

MemLinkStatSys.__str__ = memlinkstatsys_print






