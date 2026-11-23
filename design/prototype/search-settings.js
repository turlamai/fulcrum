/* Settings and launcher share the same local prototype state. */
(() => {
  const key = 'fulcrum:settings:v2';
  const defaults = { fileRoot: true, network: true, offline: true, theme: 'System', results: '12', content: false };
  const sources = [
    {id:'documents',name:'Documents',kind:'Local folder',path:'~/Documents',enabled:true,recursive:true,schedule:'On change',state:'Ready',last:'Sample index ready'},
    {id:'nas',name:'Studio NAS',kind:'Network share',path:'smb://studio-nas/shared',enabled:true,recursive:true,schedule:'Every hour',state:'Offline',last:'Cached sample results retained'},
    {id:'archive',name:'Archive drive',kind:'External drive',path:'/Volumes/Archive',enabled:true,recursive:true,schedule:'Daily',state:'Permission required',last:'Access has not been granted'},
    {id:'projects',name:'Projects',kind:'Local folder',path:'~/Projects',enabled:true,recursive:true,schedule:'On change',state:'Scan failed',last:'Sample folder could not be read'}
  ];
  let memory;
  function load() {
    if (memory) return memory;
    for (const name of ['sessionStorage', 'localStorage']) {
      try {
        const saved = JSON.parse(window[name].getItem(key));
        if (saved?.version === 2 && Array.isArray(saved.sources)) return saved;
      } catch { /* A preview may restrict storage. */ }
    }
    return null;
  }
  function save(state) {
    memory = state;
    let persisted = '';
    for (const name of ['localStorage', 'sessionStorage']) {
      try {
        window[name].setItem(key, JSON.stringify(state));
        persisted ||= name;
      } catch { /* Keep the current document usable. */ }
    }
    return persisted;
  }
  const systemTheme = matchMedia('(prefers-color-scheme: dark)');
  function isDark(theme) { return theme==='Dark' || (theme==='System' && systemTheme.matches); }
  const params = new URLSearchParams(location.search);
  function launcherUrl(query, selected) {
    const search = new URLSearchParams({q: query});
    if (selected) search.set('selected', selected);
    return 'panel.html?' + search;
  }
  function settingsUrl(query, selected, source) {
    const search = new URLSearchParams({q: query});
    if (selected) search.set('selected', selected);
    if (source) search.set('source', source);
    return 'settings.html?' + search;
  }
  window.FulcrumSearch = { defaults, sources, load, save, params, isDark, systemTheme, launcherUrl, settingsUrl };
})();
