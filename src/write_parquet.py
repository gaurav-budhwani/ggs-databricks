#!/usr/bin/env python3
"""
Reads the binary buffers produced by the C++ engine and writes
the exact Parquet tables expected by the scorer with zero-copy.
"""
import os
import sys
import struct
import pyarrow as pa
import pyarrow.parquet as pq

def main():
    scratch_dir = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("SCRATCH_DIR", "/scratch")
    out_dir = sys.argv[2] if len(sys.argv) > 2 else os.environ.get("OUTPUT_DIR", "/output")

    os.makedirs(os.path.join(out_dir, "user_daily"), exist_ok=True)
    os.makedirs(os.path.join(out_dir, "campaign_daily"), exist_ok=True)

    ud_bin = os.path.join(scratch_dir, "_ud.bin")
    if os.path.isfile(ud_bin):
        with open(ud_bin, "rb") as f:
            buf = f.read()

        hdr = struct.unpack_from("4Q", buf, 0)
        nr, u_len, c_len, _ = hdr
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
        pq.write_table(ud_table, os.path.join(out_dir, "user_daily", "part-0.parquet"))
    else:
        # Fallback to TSV
        ud_tsv = os.path.join(scratch_dir, "_ud.tsv")
        u_ids, u_days, u_sess, u_evts, u_rev, u_cats = [], [], [], [], [], []
        if os.path.isfile(ud_tsv):
            with open(ud_tsv, "r", encoding="utf-8") as f:
                for line in f:
                    parts = line.rstrip("\n").rsplit("\t", 5)
                    if len(parts) == 6:
                        u_ids.append(parts[0])
                        u_days.append(int(parts[1]))
                        u_sess.append(int(parts[2]))
                        u_evts.append(int(parts[3]))
                        u_rev.append(int(parts[4]))
                        u_cats.append(parts[5] if parts[5] else None)
        ud_schema = pa.schema([
            ("user_id", pa.string()),
            ("day", pa.date32()),
            ("sessions", pa.int64()),
            ("events", pa.int64()),
            ("revenue_cents", pa.int64()),
            ("top_category", pa.string()),
        ])
        ud_table = pa.Table.from_arrays([
            pa.array(u_ids, type=pa.string()),
            pa.array(u_days, type=pa.date32()),
            pa.array(u_sess, type=pa.int64()),
            pa.array(u_evts, type=pa.int64()),
            pa.array(u_rev, type=pa.int64()),
            pa.array(u_cats, type=pa.string()),
        ], schema=ud_schema)
        pq.write_table(ud_table, os.path.join(out_dir, "user_daily", "part-0.parquet"))

    cd_bin = os.path.join(scratch_dir, "_cd.bin")
    if os.path.isfile(cd_bin):
        with open(cd_bin, "rb") as f:
            buf = f.read()

        hdr = struct.unpack_from("4Q", buf, 0)
        nr, cid_len, chn_len, _ = hdr
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
        pq.write_table(cd_table, os.path.join(out_dir, "campaign_daily", "part-0.parquet"))
    else:
        # Fallback to TSV
        cd_tsv = os.path.join(scratch_dir, "_cd.tsv")
        c_ids, c_chns, c_days, c_sess, c_psess, c_rev, c_asst = [], [], [], [], [], [], []
        if os.path.isfile(cd_tsv):
            with open(cd_tsv, "r", encoding="utf-8") as f:
                for line in f:
                    parts = line.rstrip("\n").rsplit("\t", 6)
                    if len(parts) == 7:
                        c_ids.append(parts[0])
                        c_chns.append(parts[1])
                        c_days.append(int(parts[2]))
                        c_sess.append(int(parts[3]))
                        c_psess.append(int(parts[4]))
                        c_rev.append(int(parts[5]))
                        c_asst.append(int(parts[6]))
        cd_schema = pa.schema([
            ("campaign_id", pa.string()),
            ("channel", pa.string()),
            ("day", pa.date32()),
            ("sessions", pa.int64()),
            ("purchasing_sessions", pa.int64()),
            ("revenue_cents", pa.int64()),
            ("assisted_sessions", pa.int64()),
        ])
        cd_table = pa.Table.from_arrays([
            pa.array(c_ids, type=pa.string()),
            pa.array(c_chns, type=pa.string()),
            pa.array(c_days, type=pa.date32()),
            pa.array(c_sess, type=pa.int64()),
            pa.array(c_psess, type=pa.int64()),
            pa.array(c_rev, type=pa.int64()),
            pa.array(c_asst, type=pa.int64()),
        ], schema=cd_schema)
        pq.write_table(cd_table, os.path.join(out_dir, "campaign_daily", "part-0.parquet"))

if __name__ == "__main__":
    main()
