import pathlib,re,json,sys,collections,hashlib
p=pathlib.Path(sys.argv[1]); s=p.read_text()
lines=[x for x in s.splitlines() if x.startswith('[STORAGE] #')]
pattern=re.compile(r'^\[STORAGE\] #(\d+) seq=(\d+) boot=(\d+) exp=(\d+) .* interval_ms=(\d+) .*time=(\w+) epoch=(\d+) flags=0x([0-9A-Fa-f]+)\[.*\] crc=0x([0-9A-Fa-f]+)$')
records=[pattern.fullmatch(x) for x in lines]
assert lines and all(records), 'Malformed/missing decoded record lines'
r=[m.groups() for m in records]
seqs=[int(x[1]) for x in r]
summary=re.findall(r'^\[STORAGE\] Records read: (\d+), invalid: (\d+)$',s,re.M)
assert summary==[(str(len(r)),'0')], f'Summary mismatch {summary}'
assert [int(x[0]) for x in r]==list(range(len(r))), 'Index loss/duplication'
assert all(a<b for a,b in zip(seqs,seqs[1:])), 'Sequence reuse/reordering'
assert all(x[3]=='3' for x in r), 'Experiment mismatch'
assert s.count('[STORAGE] --- BEGIN DUMP ---')==1 and s.count('[STORAGE] --- END DUMP ---')==1
assert '[COMMAND] Firmware parsed command: ALL OK (CMD_ACK,LOGGER STORAGE DUMP)' in s
assert re.search(r'^CMD_RESULT,LOGGER STORAGE DUMP,OK$',s,re.M)
stops=re.findall(r'^\[COMMAND\] Capture ended: (.+)$',s,re.M)
assert len(stops)==1 and stops[0] in ('port quiet for 600ms.', 'transport disappeared after the successful result (expected for RELEASE).'), stops
assert not re.search(r'ERROR|WARNING|INVALID|NOT ALL OK|TRUNCATED',s)
result={'file':p.name,'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'records':len(r),'first_seq':seqs[0],'last_seq':seqs[-1],'experiment_ids':sorted(set(x[3] for x in r)),'sequence_gaps':[(a,b) for a,b in zip(seqs,seqs[1:]) if b!=a+1],'board_invalid':0,'complete':True,'capture_stop':stops[0],'boot_counts':dict(collections.Counter(x[2] for x in r)),'time_quality':dict(collections.Counter(x[5] for x in r)),'epochs':sorted(set(x[6] for x in r)),'last_records':lines[-8:]}
if len(sys.argv)>2:
    old=[x for x in pathlib.Path(sys.argv[2]).read_text().splitlines() if x.startswith('[STORAGE] #')]
    assert lines[:len(old)]==old,'Historical decoded records differ'
    result['identical_historical_record_lines']=len(old)
print(json.dumps(result,indent=2))
