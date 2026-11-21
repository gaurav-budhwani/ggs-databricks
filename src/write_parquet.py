#!/usr/bin/env python3
"""
Reads the binary buffers produced by the C++ engine and writes
the exact Parquet tables expected by the scorer with zero-copy.
"""
import os
import sys
import time
import struct
import pyarrow as pa
import pyarrow.parquet as pq
from concurrent.futures import ThreadPoolExecutor

def write_user_daily(scratch_dir, out_dir):
    ud_bin = os.path.join(scratch_dir, "_ud.bin")
    if os.path.isfile(ud_bin):
        with open(ud_bin, "rb") as f:
            buf = f.read()

        hdr = struct.unpack_from("4Q", buf, 0)
        nr, u_len, c_len, _ = hdr

        if nr == 0:
            ud_table = pa.Table.from_arrays([
                pa.array([], type=pa.string()),
                pa.array([], type=pa.date32()),
                pa.array([], type=pa.int64()),
                pa.array([], type=pa.int64()),
                pa.array([], type=pa.int64()),
                pa.array([], type=pa.string()),
            ], names=["user_id", "day", "sessions", "events", "revenue_cents", "top_category"])
            pq.write_table(ud_table, os.path.join(out_dir, "user_daily", "part-0.parquet"),
                           use_dictionary=False, row_group_size=524288)
            return

        pos = 32
        day_bytes = buf[pos : pos + nr * 4]; pos += nr * 4
        sess_bytes = buf[pos : pos + nr * 8]; pos += nr * 8
        evts_bytes = buf[pos : pos + nr * 8]; pos += nr * 8
        rev_bytes = buf[pos : pos + nr * 8]; pos += nr * 8
        u_off_bytes = buf[pos : pos + (nr + 1) * 4]; pos += (nr + 1) * 4
        u_char_bytes = buf[pos : pos + u_len]; pos += u_len
        c_bmp_len = (nr + 7) // 8
        c_bmp_bytes = buf[pos : pos + c_bmp_len]; pos += c_bmp_len
        c_off_bytes = buf[pos : pos + (nr + 1) * 4]; pos += (nr + 1) * 4
        c_char_bytes = buf[pos : pos + c_len]; pos += c_len

        arr_uid = pa.Array.from_buffers(pa.string(), nr, [None, pa.py_buffer(u_off_bytes), pa.py_buffer(u_char_bytes)])
        arr_day = pa.Array.from_buffers(pa.date32(), nr, [None, pa.py_buffer(day_bytes)])
        arr_sess = pa.Array.from_buffers(pa.int64(), nr, [None, pa.py_buffer(sess_bytes)])
        arr_evts = pa.Array.from_buffers(pa.int64(), nr, [None, pa.py_buffer(evts_bytes)])
        arr_rev = pa.Array.from_buffers(pa.int64(), nr, [None, pa.py_buffer(rev_bytes)])
        arr_cat = pa.Array.from_buffers(pa.string(), nr, [pa.py_buffer(c_bmp_bytes), pa.py_buffer(c_off_bytes), pa.py_buffer(c_char_bytes)])

        ud_table = pa.Table.from_arrays([arr_uid, arr_day, arr_sess, arr_evts, arr_rev, arr_cat],
                                     names=["user_id", "day", "sessions", "events", "revenue_cents", "top_category"])
        pq.write_table(ud_table, os.path.join(out_dir, "user_daily", "part-0.parquet"),
                       use_dictionary=False, row_group_size=524288)
    else:
        # Fallback for empty
        ud_table = pa.Table.from_arrays([
            pa.array([], type=pa.string()),
            pa.array([], type=pa.date32()),
            pa.array([], type=pa.int64()),
            pa.array([], type=pa.int64()),
            pa.array([], type=pa.int64()),
            pa.array([], type=pa.string()),
        ], names=["user_id", "day", "sessions", "events", "revenue_cents", "top_category"])
        pq.write_table(ud_table, os.path.join(out_dir, "user_daily", "part-0.parquet"),
                       use_dictionary=False, row_group_size=524288)

def write_campaign_daily(scratch_dir, out_dir):
    cd_bin = os.path.join(scratch_dir, "_cd.bin")
    if os.path.isfile(cd_bin):
        with open(cd_bin, "rb") as f:
            buf = f.read()

        hdr = struct.unpack_from("4Q", buf, 0)
        nr, cid_len, chn_len, _ = hdr

        if nr == 0:
            cd_table = pa.Table.from_arrays([
                pa.array([], type=pa.string()),
                pa.array([], type=pa.string()),
                pa.array([], type=pa.date32()),
                pa.array([], type=pa.int64()),
                pa.array([], type=pa.int64()),
                pa.array([], type=pa.int64()),
                pa.array([], type=pa.int64()),
            ], names=["campaign_id", "channel", "day", "sessions", "purchasing_sessions", "revenue_cents", "assisted_sessions"])
            pq.write_table(cd_table, os.path.join(out_dir, "campaign_daily", "part-0.parquet"),
                           use_dictionary=False, row_group_size=524288)
            return

        pos = 32
        day_bytes = buf[pos : pos + nr * 4]; pos += nr * 4
        sess_bytes = buf[pos : pos + nr * 8]; pos += nr * 8
        psess_bytes = buf[pos : pos + nr * 8]; pos += nr * 8
        rev_bytes = buf[pos : pos + nr * 8]; pos += nr * 8
        asst_bytes = buf[pos : pos + nr * 8]; pos += nr * 8
        cid_off_bytes = buf[pos : pos + (nr + 1) * 4]; pos += (nr + 1) * 4
        cid_char_bytes = buf[pos : pos + cid_len]; pos += cid_len
        chn_off_bytes = buf[pos : pos + (nr + 1) * 4]; pos += (nr + 1) * 4
        chn_char_bytes = buf[pos : pos + chn_len]; pos += chn_len

        arr_cid = pa.Array.from_buffers(pa.string(), nr, [None, pa.py_buffer(cid_off_bytes), pa.py_buffer(cid_char_bytes)])
        arr_chn = pa.Array.from_buffers(pa.string(), nr, [None, pa.py_buffer(chn_off_bytes), pa.py_buffer(chn_char_bytes)])
        arr_day = pa.Array.from_buffers(pa.date32(), nr, [None, pa.py_buffer(day_bytes)])
        arr_sess = pa.Array.from_buffers(pa.int64(), nr, [None, pa.py_buffer(sess_bytes)])
        arr_psess = pa.Array.from_buffers(pa.int64(), nr, [None, pa.py_buffer(psess_bytes)])
        arr_rev = pa.Array.from_buffers(pa.int64(), nr, [None, pa.py_buffer(rev_bytes)])
        arr_asst = pa.Array.from_buffers(pa.int64(), nr, [None, pa.py_buffer(asst_bytes)])

        cd_table = pa.Table.from_arrays([arr_cid, arr_chn, arr_day, arr_sess, arr_psess, arr_rev, arr_asst],
                                     names=["campaign_id", "channel", "day", "sessions", "purchasing_sessions", "revenue_cents", "assisted_sessions"])
        pq.write_table(cd_table, os.path.join(out_dir, "campaign_daily", "part-0.parquet"),
                       use_dictionary=False, row_group_size=524288)
    else:
        # Fallback for empty
        cd_table = pa.Table.from_arrays([
            pa.array([], type=pa.string()),
            pa.array([], type=pa.string()),
            pa.array([], type=pa.date32()),
            pa.array([], type=pa.int64()),
            pa.array([], type=pa.int64()),
            pa.array([], type=pa.int64()),
            pa.array([], type=pa.int64()),
        ], names=["campaign_id", "channel", "day", "sessions", "purchasing_sessions", "revenue_cents", "assisted_sessions"])
        pq.write_table(cd_table, os.path.join(out_dir, "campaign_daily", "part-0.parquet"),
                       use_dictionary=False, row_group_size=524288)

def main():
    scratch_dir = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("SCRATCH_DIR", "/scratch")
    out_dir = sys.argv[2] if len(sys.argv) > 2 else os.environ.get("OUTPUT_DIR", "/output")

    os.makedirs(os.path.join(out_dir, "user_daily"), exist_ok=True)
    os.makedirs(os.path.join(out_dir, "campaign_daily"), exist_ok=True)

    ready_flag = os.path.join(scratch_dir, "_ready.flag")
    while not os.path.exists(ready_flag):
        time.sleep(0.0005)

    with ThreadPoolExecutor(max_workers=2) as ex:
        f1 = ex.submit(write_user_daily, scratch_dir, out_dir)
        f2 = ex.submit(write_campaign_daily, scratch_dir, out_dir)
        f1.result()
        f2.result()

if __name__ == "__main__":
    main()
