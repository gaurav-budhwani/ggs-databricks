#!/usr/bin/env python3
"""
Reads the aggregated TSV files produced by the C++ engine and writes
the exact Parquet tables expected by the scorer.
"""
import os
import sys
import pyarrow as pa
import pyarrow.parquet as pq

def main():
    scratch_dir = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("SCRATCH_DIR", "/scratch")
    out_dir = sys.argv[2] if len(sys.argv) > 2 else os.environ.get("OUTPUT_DIR", "/output")

    os.makedirs(os.path.join(out_dir, "user_daily"), exist_ok=True)
    os.makedirs(os.path.join(out_dir, "campaign_daily"), exist_ok=True)

    ud_schema = pa.schema([
        ("user_id", pa.string()),
        ("day", pa.date32()),
        ("sessions", pa.int64()),
        ("events", pa.int64()),
        ("revenue_cents", pa.int64()),
        ("top_category", pa.string()),
    ])

    cd_schema = pa.schema([
        ("campaign_id", pa.string()),
        ("channel", pa.string()),
        ("day", pa.date32()),
        ("sessions", pa.int64()),
        ("purchasing_sessions", pa.int64()),
        ("revenue_cents", pa.int64()),
        ("assisted_sessions", pa.int64()),
    ])

    # 1. Process user_daily
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

    ud_table = pa.Table.from_arrays([
        pa.array(u_ids, type=pa.string()),
        pa.array(u_days, type=pa.date32()),
        pa.array(u_sess, type=pa.int64()),
        pa.array(u_evts, type=pa.int64()),
        pa.array(u_rev, type=pa.int64()),
        pa.array(u_cats, type=pa.string()),
    ], schema=ud_schema)
    pq.write_table(ud_table, os.path.join(out_dir, "user_daily", "part-0.parquet"))

    # 2. Process campaign_daily
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
