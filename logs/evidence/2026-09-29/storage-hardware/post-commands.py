import subprocess, pathlib, sys
root=pathlib.Path('/Users/afxjzs/dev/projects/solar-charger')
python=str(root/'.venv/bin/python')
capture='/private/tmp/bmw-hardware-capture-20260929.py'
evidence=root/'docs/evidence/2026-09-29/storage-hardware'
def run(label,options,command):
    subprocess.run([python,capture,label,'tools/send.sh',*options,*command.split()],cwd=root,check=True)
    s=(evidence/(label+'.txt')).read_text()
    assert f'Firmware parsed command: ALL OK (CMD_ACK,{command})' in s
    assert f'CMD_RESULT,{command},OK' in s
    assert not any(x in s for x in ('[STORAGE] ERROR','[STORAGE] WARNING','NOT ALL OK','TRUNCATED'))
    return s
if sys.argv[1]=='identify':
    s=run('post-version',['--wait','--timeout','150'],'VERSION')
    assert '[FIRMWARE] Revision: 1980d94\n' in s
    run('post-storage-info',['--wait','--timeout','150','--capture-seconds','10'],'LOGGER STORAGE INFO')
    s=run('post-autonomous-status',['--wait','--timeout','150'],'LOGGER AUTONOMOUS STATUS')
    assert '[AUTO] Armed: YES' in s and '[AUTO] New records will carry experiment id: 3' in s
elif sys.argv[1]=='session':
    run('post-hold',['--wait','--timeout','150'],'LOGGER SESSION HOLD')
    run('post-keepalive',[],'LOGGER SESSION KEEPALIVE')
    s=run('post-held-status',[],'STATUS')
    assert '[STATUS] Experiment ID      = 3' in s
    run('post-session-status',[],'LOGGER SESSION STATUS')
    run('post-release',[],'LOGGER SESSION RELEASE')
else: raise SystemExit('Unknown stage')
