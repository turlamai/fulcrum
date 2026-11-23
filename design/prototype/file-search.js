/* Sample index only. Paths identify fixtures and are never opened by the OS. */
(() => {
  const search = window.FulcrumSearch;
  const P = window.Prototype;
  const panel = document.getElementById('app');
  const list = panel.querySelector('.list');
  const query = panel.querySelector('.query .q');
  const template = list.querySelector('[data-file-template]');
  const saved = search.load();
  const sources = saved?.sources ?? search.sources;
  const options = {...search.defaults, ...saved?.values};
  let theme = options.theme;
  function applyTheme() {
    document.documentElement.classList.toggle('dark',search.isDark(theme));
    panel.classList.remove('dark');
  }
  document.getElementById('themeBtn').onclick = () => {
    theme = search.isDark(theme) ? 'Light' : 'Dark';
    search.save({...saved, version:2, sources, values:{...options, theme}});
    applyTheme();
  };
  search.systemTheme.addEventListener('change',applyTheme);
  applyTheme();
  const files = [
    {id:'quote-local', name:'Project Quote.pdf', path:'~/Documents/Quotes/Project Quote.pdf', version:'Working draft · revised Nov 18', content:'Fixture: installation and hardware estimate for the lobby renovation.'},
    {id:'quote-nas', name:'Project Quote.pdf', path:'smb://studio-nas/shared/Bids/Project Quote.pdf', version:'Shared version · revised Nov 21', content:'Fixture: approved estimate for the lobby renovation and support.'},
    {id:'quote-bids', name:'Project Quote.pdf', path:'smb://studio-nas/bids/Project Quote.pdf', version:'Bid library copy · revised Nov 19', content:'Fixture: supplier bid covering freight and equipment.'},
    {id:'quote-archive', name:'Project Quote.pdf', path:'/Volumes/Archive/2025/Project Quote.pdf', version:'Archived version · revised Dec 12, 2025', content:'Fixture: historical estimate for the south wing.'},
    {id:'tokens', name:'figma-tokens.json', path:'~/Documents/Fulcrum/design/figma-tokens.json', version:'Design token reference', content:'Fixture: semantic color tokens for focus and selection.'}
  ];
  const normalize = path => path.replace(/\\/g, '/').replace(/^\/\//, 'smb://').replace(/\/+$/, '');
  function matchingSource(file) {
    const path = normalize(file.path);
    return sources.filter(source => {
      if (!source.enabled || (!options.network && source.kind !== 'Local folder')) return false;
      if (source.state !== 'Ready' && (source.state !== 'Offline' || !options.offline)) return false;
      const root = normalize(source.path);
      if (!path.startsWith(root + '/')) return false;
      const relative = path.slice(root.length + 1);
      return source.recursive || !relative.includes('/');
    }).sort((a,b) => b.path.length - a.path.length)[0];
  }
  const patterns = (saved?.rules ?? []).flatMap(rule => rule.pattern.split(',')).map(value => value.trim()).filter(Boolean);
  function ignored(file, source) {
    const path = normalize(file.path);
    const relative = path.slice(normalize(source.path).length + 1);
    return patterns.some(pattern => {
      const normalized = normalize(pattern);
      const safe = normalized.split(/(\*\*\/|\*\*|\*|\?)/).map(part => {
        if (part === '**/') return '(?:.*/)?';
        if (part === '**') return '.*';
        if (part === '*') return '[^/]*';
        if (part === '?') return '[^/]';
        return part.replace(/[|\\{}()[\]^$+.]/g, '\\$&');
      }).join('');
      if (!safe) return false;
      const match = new RegExp('^' + safe + '(?:/|$)', 'i');
      return [path, relative, file.name].some(value => match.test(value));
    });
  }
  const results = new Map();
  if (options.fileRoot) {
    for (const file of files) {
      const source = matchingSource(file);
      if (!source || ignored(file, source)) continue;
      results.set(file.id, {...file, source});
      const row = template.cloneNode(true);
      row.removeAttribute('data-file-template');
      row.dataset.fileId = file.id;
      row.tabIndex = -1;
      const matchNote = document.createElement('div');
      matchNote.className = 'sub match-note';
      matchNote.hidden = true;
      row.querySelector('.txt').append(matchNote);
      row.querySelector('.ttl').textContent = file.name;
      row.querySelector('.sub').textContent = source.name + ' · ' + file.path.slice(0, file.path.lastIndexOf('/'));
      row.querySelector('.badge').textContent = source.state === 'Offline' ? 'Offline' : source.kind === 'Local folder' ? 'Local' : source.kind;
      template.before(row);
    }
  }
  template.remove();
  const previous = search.params.get('q');
  if (previous !== null) query.textContent = previous;
  function settingsUrl(source) {
    const selected = panel.querySelector('.row.selected');
    return search.settingsUrl(P.text(query), selected?.dataset.fileId || selected?.id, source);
  }
  const needleFor = input => input.replace(/^find(?:\s+|$)/, '').trim();
  function alternate(file, needle) {
    if (file.id.startsWith('quote') && '报价单'.includes(needle)) return '报价单';
    if (options.content && file.source.state==='Ready' && file.content.toLowerCase().includes(needle)) return file.content;
    return '';
  }
  function matches(row, input) {
    const file = results.get(row.dataset.fileId);
    if (!file) return !/^find(?:\s|$)/i.test(input) && ['.ttl','.sub','.badge']
      .some(field=>P.text(row.querySelector(field)).toLowerCase().includes(input));
    const needle = needleFor(input);
    return [file.name,file.path,file.source.name].some(value=>value.toLowerCase().includes(needle))
      || Boolean(alternate(file,needle));
  }
  function highlight(node, value, needle) {
    if (!node) return false;
    const index = needle ? value.toLowerCase().indexOf(needle) : -1;
    if (index<0) { node.textContent=value; return false; }
    const hit = document.createElement('mark');
    hit.className = 'qhit';
    hit.textContent = value.slice(index,index+needle.length);
    node.replaceChildren(value.slice(0,index),hit,value.slice(index+needle.length));
    return true;
  }
  function mark(row, input) {
    const needle = needleFor(input);
    const title = row.querySelector('.ttl');
    const subtitle = row.querySelector('.sub');
    const badge = row.querySelector('.badge');
    const file = results.get(row.dataset.fileId);
    const titleHit = highlight(title,title.textContent,needle);
    const pathHit = highlight(subtitle,subtitle.textContent,needle);
    if (!file) { highlight(badge,P.text(badge),needle); return; }
    const note = row.querySelector('.match-note');
    const text = needle && !titleHit && !pathHit ?
      alternate(file,needle) || (file.path.toLowerCase().includes(needle)?file.path:'') : '';
    note.hidden = !text;
    highlight(note,text,needle);
  }
  const dialog = document.createElement('dialog');
  dialog.className = 'file-dialog';
  dialog.setAttribute('aria-labelledby','file-title');
  panel.append(dialog);
  let lastRow;
  let returnToActions = false;
  dialog.addEventListener('close', () => {
    returnToActions = false;
    (lastRow && !lastRow.hidden ? lastRow : query).focus({preventScroll:true});
  });
  dialog.addEventListener('cancel', event=>{
    if (!returnToActions) return;
    event.preventDefault();
    returnToActions = false;
    open(lastRow,true);
  });
  function open(row, detailOnly = false) {
    if (!row || row.hidden) return false;
    lastRow = row;
    const file = results.get(row.dataset.fileId);
    if (!file) {
      if (!detailOnly) return false;
      const title = P.text(row.querySelector('.ttl'));
      const description = P.text(row.querySelector('.sub'));
      const actionLabel = row.dataset.actionLabel || 'Run sample';
      dialog.innerHTML = `<p class="file-context">Actions</p><h2 id="file-title">${P.esc(title)}</h2>
        <p class="file-explanation">${P.esc(description)}</p><div class="file-actions">
        <button type="button" data-run>${P.esc(actionLabel)}</button><button type="button" data-close>Back to results</button></div>`;
      dialog.querySelector('[data-close]').onclick = ()=>dialog.close();
      dialog.querySelector('[data-run]').onclick = ()=>{
        const old = dialog.querySelector('[data-run]');
        const feedback = document.createElement('p');
        feedback.setAttribute('role','status');
        feedback.textContent = row.dataset.actionResult || 'Sample action complete.';
        old.closest('.file-actions').before(feedback);
        dialog.querySelector('[data-close]').focus();
        old.remove();
      };
      dialog.showModal();
      dialog.querySelector('[data-run]').focus();
      return true;
    }
    const offline = file.source.state === 'Offline';
    const detail = detailOnly || offline;
    dialog.innerHTML = `<p class="file-context">${detail ? (detailOnly?'Actions · File details':'File details') : 'Sample preview'}</p>
      <h2 id="file-title">${P.esc(file.name)}</h2>
      <dl><dt>Source</dt><dd>${P.esc(file.source.name)} · ${P.esc(file.source.kind)}</dd>
      <dt>Path</dt><dd><code>${P.esc(file.path)}</code></dd><dt>Version</dt><dd>${P.esc(file.version)}</dd></dl>
      ${offline ? '<p class="file-explanation">This share is offline. The index retains the file record, but the document is unavailable until the share reconnects.</p>' : !detail ? '<p class="file-explanation">This sample preview shows which file you selected.</p>' : ''}
      ${!detail ? `<p class="file-explanation">${P.esc(file.content)}</p>` : ''}
      <div class="file-actions">
        <button type="button" data-close>${detail ? 'Back to results' : returnToActions?'Back to actions':'Close preview'}</button>
        ${detail ? `<a href="${P.esc(settingsUrl(file.source.id))}">${offline ? 'Reconnect in Settings' : 'Manage search location'}</a>` : ''}
        ${detail && !offline ? '<button type="button" data-preview>Open sample</button>' : ''}
      </div>`;
    dialog.querySelector('[data-close]').onclick = () => {
      if (returnToActions) { returnToActions=false; open(row,true); }
      else dialog.close();
    };
    const preview = dialog.querySelector('[data-preview]');
    if (preview) preview.onclick = () => { returnToActions=true; open(row); };
    if (!dialog.open) dialog.showModal();
    (detail ? (dialog.querySelector('[data-preview]') || dialog.querySelector('a')) : dialog.querySelector('[data-close]')).focus();
    return true;
  }
  window.FulcrumFiles = { panel, open, resultLimit:Number(options.results), matches, mark, settingsUrl, previous, selected: search.params.get('selected') };
  addEventListener('pageshow', event => { if (event.persisted) location.reload(); });
})();
