/* Fulcrum query and keyboard prototype; OS integrations remain explicit examples. */
(() => {
 const { $, $$, text, link, match, go, modal, status, copy }=Prototype;
 const files=window.FulcrumFiles;
 const resident=window.FulcrumResident;
 function selectRows(rows,selected){
   rows.forEach(row=>{
     row.classList.toggle('selected',row===selected);
     row.setAttribute('aria-current',row===selected);
     if(files) row.tabIndex=row===selected?0:-1;
   });
 }
 const context=document.createElement('nav');context.className='proto-context';context.setAttribute('aria-label','Fulcrum 原型');context.innerHTML='<a href="panel.html">主面板</a><a href="settings.html">设置</a><a href="audit.html">状态对照</a>';document.body.prepend(context);
 $$('.query .q').forEach(q=>{
   q.querySelector('.caret')?.remove();
   q.contentEditable='true';
   q.setAttribute('role','searchbox');
   q.setAttribute('aria-label',files?'Search applications and files':'搜索示例结果');
   q.setAttribute('spellcheck','false');
   const panel=q.closest('.panel');
   if(!panel)return;
   const rows=$$('.list .row',panel);
   const resultsList=$('.list',panel);
   let empty, composing=false;
   function filter(){
     if(composing)return;
     const input=text(q).toLowerCase();
     const selection=rows.find(r=>r.classList.contains('selected'));
     const visible=rows.filter(row=>{
       row.hidden=!(files?files.matches(row,input):text(row).toLowerCase().includes(input));
       if(files)files.mark(row,input);
       return !row.hidden;
     });
     if(files){
       const selected=visible.includes(selection)?selection:visible[0];
       selectRows(rows,selected);
       const count=visible.length===1?'1 result':visible.length+' results';
       $('.foot .status',panel).textContent=count+(visible.length>files.resultLimit?' · scroll for more':'');
       if(visible.length){
         const shown=visible.slice(0,files.resultLimit || 12);
         resultsList.style.maxHeight=shown.reduce((height,row)=>height+(row.querySelector('.match-note:not([hidden])')?56:44),14)+'px';
       }
     }
     if(!empty){
       empty=document.createElement('p');
       empty.className='proto-result';
       empty.textContent=files?'No results in your enabled search locations. Try another term or review File Search in Settings.':'没有匹配的示例结果';
       $('.list',panel)?.after(empty);
     }
     empty.hidden=visible.length>0;
   }
   q.addEventListener('compositionstart',()=>{composing=true});
   q.addEventListener('compositionend',()=>{composing=false;filter()});
   q.addEventListener('input',filter);
   if(files){
     rows.filter(row=>!row.dataset.fileId).forEach((row,i)=>{row.id='launcher-item-'+i});
     const preferred=rows.find(row=>(row.dataset.fileId||row.id)===files.selected);
     if(preferred)selectRows(rows,preferred);
     filter();
     if(!panel.hidden){
       q.focus({preventScroll:true});
       const selected=$('.row.selected',panel);
       selected?.scrollIntoView({block:'nearest'});
     }
   }
 });
 $$('.panel').forEach(panel=>{
   const rows=$$('.list .row',panel);rows.forEach(row=>link(row,()=>{if(row.classList.contains('disabled'))return;selectRows(rows,row);if(files?.open(row)||files?.open(row,true))return;const title=text($('.ttl',row));
     if(/Open Settings|web search in Settings/.test(title)){go('settings.html');return}
     if(/Delete from history/i.test(title)){modal('Delete local sample?','Only this sample row will be removed.',[],()=>row.remove(),'Delete');return}
     if(/^Pin$/.test(title)){row.classList.toggle('pinned');status(row.classList.contains('pinned')?'Sample pinned':'Sample unpinned');return}
     if(/^Copy$|^\d+\s*[×+−]/.test(title)){copy(title==='Copy'?'--accent-soft: #faeee5; --line: #e7e3da;':title.split('=').at(-1).trim());return}
     if(/Retry/.test(title)){status('演示请求保持失败；请先在设置配置演示提供商');return}
     modal(title,text($('.sub',row))+'\n原型演示：不打开本机应用、文件或网页。',[],()=>go('actions.html'),'查看操作示例');
   }));
   if(files)selectRows(rows,rows.find(row=>row.classList.contains('selected')));
   panel.addEventListener('keydown',e=>{
     if(e.isComposing||e.keyCode===229||e.target.closest('dialog,input,select,textarea,button,a'))return;
     if(files&&(e.metaKey||e.ctrlKey)&&e.key.toLowerCase()==='k'&&files.open($('.row.selected',panel),true)){e.preventDefault();return}
     if(e.metaKey||e.ctrlKey||e.altKey)return;
     const visible=rows.filter(r=>!r.hidden&&!r.classList.contains('disabled'));let i=visible.findIndex(r=>r.classList.contains('selected'));
     if((e.key==='ArrowDown'||e.key==='ArrowUp')&&visible.length){e.preventDefault();i=i<0?(e.key==='ArrowDown'?0:visible.length-1):(i+(e.key==='ArrowDown'?1:-1)+visible.length)%visible.length;selectRows(rows,visible[i]);if(files&&e.target.matches('[contenteditable]'))visible[i]?.scrollIntoView({block:'nearest'});else visible[i]?.focus()}
     if(e.key==='Enter'&&!e.target.closest('.row')){e.preventDefault();(visible[i]||visible[0])?.click()}
     if(e.key==='Escape'){
       e.preventDefault();
       const slot=$('.slot:not([hidden])',panel);
       if(slot){slot.hidden=true;return}
       const q=$('.query .q',panel);
       if(q&&(!files||text(q))){q.textContent='';q.dispatchEvent(new Event('input'));q.focus()}
       else if(files)resident.hide();
     }
     if(!files&&e.key==='Tab'&&!e.shiftKey&&e.target.matches('[contenteditable]')&&visible.length){e.preventDefault();const q=e.target;q.textContent=text($('.ttl',visible[i]||visible[0]));q.dispatchEvent(new Event('input'))}
   });
 });
 match('button',/^esc$/,()=>go('panel.html'));
 match('button',/^Retry/,()=>status('演示请求失败状态仍保留 · 原型未连接模型服务'));
 match('button',/^Turn on web search/,()=>go('settings.html'));
 match('button',/^ai figglewump/,()=>go('ai.html'));
 if(files)context.querySelector('a[href="settings.html"]').addEventListener('click',e=>{
   e.preventDefault();
   go(resident.settingsLink());
 });
 document.addEventListener('keydown',e=>{
   if(e.defaultPrevented||e.isComposing||e.keyCode===229||e.target.closest('dialog'))return;
   if((e.metaKey||e.ctrlKey)&&e.key===','){e.preventDefault();go(files?resident.settingsLink():'settings.html')}
   if((e.metaKey||e.ctrlKey)&&e.key.toLowerCase()==='k'){e.preventDefault();if(!files)go('actions.html')}
 });
})();
