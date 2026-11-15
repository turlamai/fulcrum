/* Fulcrum query and keyboard prototype; OS integrations remain explicit examples. */
(() => {
 const P=Prototype,{ $, $$, text, link, match, go, modal, status, copy, read, save }=P;
 const file=location.pathname.split('/').pop();
 const context=document.createElement('nav');context.className='proto-context';context.setAttribute('aria-label','Fulcrum 原型');context.innerHTML='<a href="panel.html">主面板</a><a href="settings.html">设置</a><a href="audit.html">状态对照</a>';document.body.prepend(context);
 $$('.query .q').forEach(q=>{q.querySelector('.caret')?.remove();q.contentEditable='true';q.setAttribute('role','searchbox');q.setAttribute('aria-label','搜索示例结果');q.setAttribute('spellcheck','false');const panel=q.closest('.panel');if(!panel)return;const rows=$$('.list .row',panel);let empty; q.addEventListener('input',()=>{const query=text(q).toLowerCase();rows.forEach(r=>r.hidden=!text(r).toLowerCase().includes(query));if(!empty){empty=document.createElement('p');empty.className='proto-result';empty.textContent='没有匹配的示例结果';$('.list',panel)?.after(empty)}empty.hidden=rows.some(r=>!r.hidden)});});
 $$('.panel').forEach(panel=>{
   const rows=$$('.list .row',panel);rows.forEach(row=>link(row,()=>{if(row.classList.contains('disabled'))return;rows.forEach(r=>{r.classList.toggle('selected',r===row);r.setAttribute('aria-current',r===row)});const title=text($('.ttl',row));
     if(/Open Settings|web search in Settings/.test(title)){go('settings.html');return}
     if(/Delete from history/i.test(title)){modal('Delete local sample?','Only this sample row will be removed.',[],()=>row.remove(),'Delete');return}
     if(/^Pin$/.test(title)){row.classList.toggle('pinned');status(row.classList.contains('pinned')?'Sample pinned':'Sample unpinned');return}
     if(/^Copy$|^\d+\s*[×+−]/.test(title)){copy(title==='Copy'?'--accent-soft: #faeee5; --line: #e7e3da;':title.split('=').at(-1).trim());return}
     if(/Retry/.test(title)){status('演示请求保持失败；请先在设置配置演示提供商');return}
     modal(title,text($('.sub',row))+'\n原型演示：不打开本机应用、文件或网页。',[],()=>go('actions.html'),'查看操作示例');
   }));
   panel.addEventListener('keydown',e=>{
     if(e.target.closest('dialog,input,select,textarea')||e.metaKey||e.ctrlKey||e.altKey)return;
     const visible=rows.filter(r=>!r.hidden&&!r.classList.contains('disabled'));let i=visible.findIndex(r=>r.classList.contains('selected'));
     if(e.key==='ArrowDown'||e.key==='ArrowUp'){e.preventDefault();i=(i+(e.key==='ArrowDown'?1:-1)+visible.length)%visible.length;rows.forEach(r=>{r.classList.toggle('selected',r===visible[i]);r.setAttribute('aria-current',r===visible[i])});visible[i]?.focus()}
     if(e.key==='Enter'){e.preventDefault();(visible[i]||visible[0])?.click()}
     if(e.key==='Escape'){e.preventDefault();const slot=$('.slot',panel);if(slot){slot.hidden=true;return}const q=$('.query .q',panel);if(q){q.textContent='';q.dispatchEvent(new Event('input'));q.focus()}}
     if(e.key==='Tab'&&e.target.matches('[contenteditable]')){e.preventDefault();const q=e.target;q.textContent=text($('.ttl',visible[i]||visible[0]));q.dispatchEvent(new Event('input'))}
   });
 });
 match('button',/^esc$/,()=>go('panel.html'));
 match('button',/^Retry/,()=>status('演示请求失败状态仍保留 · 原型未连接模型服务'));
 match('button',/^Turn on web search/,()=>go('settings.html'));
 match('button',/^ai figglewump/,()=>go('ai.html'));
 if(file==='settings.html'){
   P.switches('.switch');
   $$('.stepbtn').forEach(b=>link(b,()=>{const v=$('.v',b.parentElement);v.textContent=Math.min(12,Math.max(1,+text(v)+(text(b)==='+'?1:-1)));save('maxResults',Number(text(v)))}));if($('.step .v'))$('.step .v').textContent=read('maxResults',12);
   $$('.sitem').forEach(el=>link(el,()=>{const target=text(el);const headers=$$('.form .sect');const section=target==='AI'?headers.find(x=>text(x)==='AI'):target==='General'?headers[0]:null;if(section){section.scrollIntoView({block:'start'});return}modal(target,'此页仅实现 General 与 AI 的设置样例；其他分类为设计范围参考。')}));
   $$('.picker').forEach((select,i)=>{const saved=read('picker'+i,null);if(saved&&[...select.options].some(o=>o.value===saved))select.value=saved;select.addEventListener('change',()=>{save('picker'+i,select.value);if(i===1){const dark=select.value==='Dark'||select.value==='System'&&matchMedia('(prefers-color-scheme:dark)').matches;$('#app').classList.toggle('dark',dark);save('dark',dark)}else status('原型选项已记录；不更改系统设置')})});
   match('button',/^Save key/,()=>modal('演示 API key','仅验证 demo-key，不保存任何密钥。',[{label:'演示密钥',name:'key',required:true,type:'password'}],d=>{if(d.key!=='demo-key'){status('请输入 demo-key');return false}status('演示凭据验证通过 · 未保存密钥')},'验证'));
   match('button',/Record/,b=>{const old=text(b);b.textContent='Press shortcut…';const record=e=>{if(e.key==='Escape'){b.textContent=old;document.removeEventListener('keydown',record,true);return}if(!e.metaKey&&!e.ctrlKey&&!e.altKey)return;if(['Meta','Control','Alt','Shift'].includes(e.key))return;e.preventDefault();e.stopImmediatePropagation();const combo=[e.metaKey?'⌘':'',e.ctrlKey?'Ctrl+':'',e.altKey?'Alt+':'',e.shiftKey?'Shift+':'',e.key===' '?'Space':e.key.toUpperCase()].join('');b.textContent=combo+' — Record';save('shortcut',combo);document.removeEventListener('keydown',record,true)};document.addEventListener('keydown',record,true)});
 }
 document.addEventListener('keydown',e=>{if((e.metaKey||e.ctrlKey)&&e.key===','){e.preventDefault();go('settings.html')}if((e.metaKey||e.ctrlKey)&&e.key.toLowerCase()==='k'&&!e.target.closest('dialog')){e.preventDefault();go('actions.html')}});
})();
