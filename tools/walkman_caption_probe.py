#!/usr/bin/env python3
"""Check timestamped three-row pages using cloud stories, with the microphone off."""
import argparse,sys,time,json
from pathlib import Path
root=Path(__file__).resolve().parents[1]
from walkman_online_device import OnlineDevice
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--port',required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args();out=args.output;out.mkdir(parents=True,exist_ok=True)
d=OnlineDevice(args.port)
r={'passed':False,'microphone_used':False,'observations':[],'rounds':[],'page_events':[]}
original_line=d.line
def line(deadline):
 text=original_line(deadline)
 if text.startswith('WM_PAGE '):
  try:r['page_events'].append(json.loads(text[8:]))
  except ValueError:pass
 return text
d.line=line
def read():
 for i in range(4):
  try:s=d.state();break
  except TimeoutError:
   if i==3:raise
 r['observations'].append({'t':round(time.monotonic(),3),'state':s})
 if s['net']['error_code']:raise RuntimeError('device error '+str(s['net']))
 assert not s['net']['recording']
 return s
try:
 end=time.monotonic()+35
 while True:
  s=read()
  if s['net']['wifi'] and s['net']['phase']==2:break
  if time.monotonic()>end:raise TimeoutError('WiFi')
  time.sleep(.2)
 baseline=s['net'].copy()
 for turn in range(2):
  prepared=s['net'].get('prepared',False);start=time.monotonic();old=s['net']['reply_id'];d.serial.write(b'j');sequence=[];first=None;first_text=None;prev=None;end=start+85
  while time.monotonic()<end:
   s=read();n=s['net']
   if n['reply_id']<=old:continue
   if n['text_bytes'] and first_text is None:first_text=round(time.monotonic()-start,2)
   if not n['reply_played']:continue
   if n['phase']==5 and first is None:first=round(time.monotonic()-start,2)
   if not sequence or sequence[-1]['page']!=n['caption_page']:
    sequence.append({'page':n['caption_page'],'audio_s':round(n['reply_played']/24000,3),'previous_boundary_s':None if not prev or prev['caption_next_sample']==4294967295 else prev['caption_next_sample']/24000,'next':n['caption_next_sample']})
    if len(sequence)>1:
     assert n['caption_page']==sequence[-2]['page']+1
     if prev['caption_next_sample']!=4294967295:assert n['reply_played']>=prev['caption_next_sample']
    print('page',turn,sequence[-1],flush=True)
   assert n['failures']==baseline['failures'] and n['dropped']==baseline['dropped'] and n['starves']==baseline['starves']
   prev=n
   if n['reply_played'] and n['phase']==2:break
   time.sleep(.12)
  else:raise TimeoutError('reply')
  assert n['caption_pages']>=2 and n['caption_page']==n['caption_pages']-1
  rr={'round':turn,'reply_id':n['reply_id'],'first_audio_s':first,'first_text_s':first_text,'prepared':prepared,'audio_s':n['reply_played']/24000,'pages':n['caption_pages'],'sequence':sequence,'starves':n['starves']-baseline['starves'],'heap':s.get('heap')}
  r['rounds'].append(rr);print(json.dumps(rr),flush=True)
  # Keep the response on the last page after playback and TLS cleanup.
  last=n['caption_page'];deadline=time.monotonic()+4
  while time.monotonic()<deadline:
   s=read();assert s['net']['caption_page']==last;time.sleep(.15)
  assert not s['net']['connected'] or s['net'].get('prepared',False)
  if turn==0:d.capture(out/'last-page.png')
 r['final']=s
 tested={item['reply_id'] for item in r['rounds']}
 checked=set()
 for e in r['page_events']:
  pair=(e['reply'],e['page'])
  if e['reply'] in tested and e['page']>0 and not e['manual'] and pair not in checked:
   checked.add(pair)
   assert e['sample']>=e['cue']
   assert (e['sample']-e['cue'])/24000<.7,(e,'page delayed')
 assert len(checked)==sum(item['pages']-1 for item in r['rounds'])
 r['passed']=True
finally:
 (out/'caption-validation.json').write_text(json.dumps(r,indent=2));d.serial.close()
