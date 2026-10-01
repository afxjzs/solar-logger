import sys, subprocess, pathlib, datetime, json, hashlib, os
repo = pathlib.Path('/Users/afxjzs/dev/projects/solar-charger')
out = repo/'docs/evidence/2026-09-29/storage-hardware'
out.mkdir(exist_ok=True)
label = sys.argv[1]
args = sys.argv[2:]
assert label.replace('-', '').isalnum()
assert args and args[0] in ('tools/send.sh', 'tools/upload.sh')
if args[0] == 'tools/send.sh':
    cmd = ' '.join(args)
    assert not any(x in cmd for x in ('CLEAR','RESET',' OFF',' ON','INTERVAL'))
raw=out/(label+'.txt')
meta=out/(label+'.json')
assert not raw.exists() and not meta.exists(), 'Refusing overwrite'
start = datetime.datetime.now().astimezone().isoformat()
env=os.environ.copy(); env['PATH']='/Users/afxjzs/.local/bin:/opt/homebrew/bin:'+env['PATH']; env['PYTHONUNBUFFERED']='1'
with raw.open('xb') as f:
    p=subprocess.Popen(args, cwd=repo, env=env, stdout=f, stderr=subprocess.STDOUT)
    print(f'Capturing {label}; child PID {p.pid}; started {start}', flush=True)
    rc=p.wait()
metadata={'started':start,'finished':datetime.datetime.now().astimezone().isoformat(),'argv':args,'exit_code':rc,'bytes':raw.stat().st_size,'sha256':hashlib.sha256(raw.read_bytes()).hexdigest()}
meta.write_text(json.dumps(metadata,indent=2)+'\n')
print(json.dumps(metadata,indent=2),flush=True)
if raw.stat().st_size < 20000: print(raw.read_text(errors='replace'))
else: print(raw.read_text(errors='replace')[-3500:])
sys.exit(rc)
