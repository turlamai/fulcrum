/* Shared, deterministic updater simulation for the launcher and Settings. */
(() => {
  const key = 'fulcrum:prototype:update';
  const params = new URLSearchParams(location.search);
  const initial = {phase:'idle', installed:'1.4.0', scenario:'available'};
  let state = initial;
  for (const name of ['sessionStorage','localStorage']) {
    try {
      const saved = JSON.parse(window[name].getItem(key));
      if (saved?.phase) { state = saved; break; }
    } catch { /* Storage may be unavailable inside previews. */ }
  }
  const outcome = params.get('updateScenario');
  if (['available','current','error'].includes(outcome)) {
    state = {...initial, scenario:outcome};
  }
  const labels = {
    idle:'Check for updates', checking:'Checking for updates…', current:'You’re up to date',
    available:'Version 1.5.0 is available', error:'Couldn’t check for updates',
    installing:'Installing update…', restarting:'Restarting Fulcrum…', installed:'Update installed'
  };
  let timer;
  function publish() {
    for (const name of ['sessionStorage','localStorage']) {
      try { window[name].setItem(key,JSON.stringify(state)); } catch { /* In-memory fallback. */ }
    }
    document.dispatchEvent(new Event('fulcrum-update'));
    clearTimeout(timer);
    if (state.until) timer = setTimeout(advance, Math.max(0,state.until-Date.now()));
  }
  function set(phase, duration = 0, extra = {}) {
    state = {...state, ...extra, phase, until:duration ? Date.now()+duration : 0};
    publish();
  }
  function advance() {
    if (state.phase==='checking') set(state.installed==='1.5.0'?'current':state.scenario);
    else if (state.phase==='installing') set('restarting',700);
    else if (state.phase==='restarting') set('installed',0,{installed:'1.5.0'});
  }
  function check() {
    if (state.until || state.phase==='available') return;
    if (state.phase==='error') set('checking',850,{scenario:'available'});
    else set('checking',850);
  }
  function render(root) {
    if (!root) return;
    const {phase, installed} = state;
    const prior = root.contains(document.activeElement);
    const message = {
      idle:'Look for a newer version of Fulcrum.',
      checking:'Looking for a newer version.',
      current:'This sample installation is on the latest version.',
      available:'This sample update improves file search and recovery for disconnected locations.',
      error:'The update server could not be reached. Check your connection, then try again.',
      installing:'Preparing the new version. Your settings and search context are kept.',
      restarting:'Returning Fulcrum to its previous window state.',
      installed:'Fulcrum is back on the new version.'
    }[phase];
    root.innerHTML = `<p class="update-version">Installed version <b>${installed}</b></p>
      <div class="update-state" tabindex="-1" role="status" aria-live="polite"><strong>${labels[phase]}</strong><p>${message}</p></div>
      ${state.until ? '' : `<button type="button" class="update-button${phase==='available'?' is-primary':''}">${phase==='available'?'Install and restart':phase==='error'?'Retry':'Check for updates'}</button>`}`;
    const button = root.querySelector('button');
    if (button) button.onclick = phase==='available'?()=>set('installing',1000):check;
    if (prior) (button || root.querySelector('[role=status]')).focus();
  }
  window.FulcrumUpdates = {check, render, label:()=>labels[state.phase], phase:()=>state.phase};
  publish();
})();
