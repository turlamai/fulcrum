/* Desktop chrome simulation. Both platform presentations use the same commands. */
(() => {
  const host = document.getElementById('desktop-status');
  const panel = document.getElementById('app');
  const query = panel.querySelector('.query .q');
  const menu = document.getElementById('resident-menu');
  const trigger = document.getElementById('resident-trigger');
  const launcher = document.getElementById('relaunch');
  const updateDialog = document.getElementById('resident-update');
  const updates = window.FulcrumUpdates;
  const params = window.FulcrumSearch.params;
  let platform = params.get('platform')==='mac'?'mac':'win';
  let running = true;
  let visible = params.get('resident')!=='1';

  function paint() {
    host.dataset.platform = platform;
    panel.hidden = !visible;
    trigger.hidden = !running;
    launcher.hidden = running;
    host.querySelector('.resident-location').textContent = platform==='mac'?'macOS menu bar':'Windows notification area';
    host.querySelectorAll('[data-platform-choice]').forEach(button=>{
      button.setAttribute('aria-pressed',button.dataset.platformChoice===platform);
    });
    document.querySelectorAll('[data-hide-launcher]').forEach(button=>button.disabled=!visible);
    host.querySelector('.resident-state').textContent = running ? visible?'Launcher open':'Running in background' : 'Fulcrum has quit';
    trigger.dataset.update = updates.phase()==='available'?'available':'';
    trigger.setAttribute('aria-label',updates.phase()==='available'?'Fulcrum menu, update available':'Fulcrum menu');
    menu.querySelector('[data-resident-command="update"]').textContent = ({
      available:'View available update…', error:'Review update error…', checking:'Checking for updates…',
      installing:'View installation…', restarting:'View restart…'
    })[updates.phase()] || 'Check for updates…';
  }
  function closeMenu(restore = false) {
    menu.hidden = true;
    trigger.setAttribute('aria-expanded','false');
    if (restore && running) trigger.focus();
  }
  function openMenu(last = false) {
    if (!running) return;
    paint();
    menu.hidden = false;
    trigger.setAttribute('aria-expanded','true');
    const buttons = [...menu.querySelectorAll('button')];
    (last ? buttons.at(-1) : buttons[0]).focus();
  }
  function restore() {
    running = true;
    visible = true;
    closeMenu();
    paint();
    query.focus();
  }
  function hide() {
    if (!running) return;
    visible = false;
    closeMenu();
    paint();
    trigger.focus();
  }
  function settingsLink(page) {
    const url = new URL(window.FulcrumFiles.settingsUrl(),location.href);
    url.searchParams.set('platform',platform);
    if (!visible) url.searchParams.set('resident','1');
    if (page) url.searchParams.set('page',page);
    return url.href;
  }
  trigger.addEventListener('click',()=>menu.hidden?openMenu():closeMenu(true));
  trigger.addEventListener('contextmenu',event=>{event.preventDefault();openMenu();});
  trigger.addEventListener('keydown',event=>{
    if (event.key!=='ArrowDown' && event.key!=='ArrowUp') return;
    event.preventDefault();
    openMenu(event.key==='ArrowUp');
  });
  menu.addEventListener('keydown',event=>{
    const buttons = [...menu.querySelectorAll('button')];
    if (event.key==='Tab') { closeMenu(true); return; }
    if (event.key==='Escape') { event.preventDefault(); closeMenu(true); return; }
    if (!['ArrowDown','ArrowUp','Home','End'].includes(event.key)) return;
    event.preventDefault();
    const current = buttons.indexOf(document.activeElement);
    const next = event.key==='Home'?0:event.key==='End'?buttons.length-1:
      (current+(event.key==='ArrowDown'?1:-1)+buttons.length)%buttons.length;
    buttons[next].focus();
  });
  menu.addEventListener('click',event=>{
    const command = event.target.closest('[data-resident-command]')?.dataset.residentCommand;
    if (command==='open') restore();
    if (command==='settings') location.href=settingsLink();
    if (command==='update') {
      closeMenu();
      const root = updateDialog.querySelector('.shared-update');
      updates.render(root);
      document.getElementById('resident-update-settings').href=settingsLink('sync');
      updateDialog.showModal();
      if (['idle','current','installed'].includes(updates.phase())) updates.check();
      root.querySelector('[role=status]').focus();
    }
    if (command==='quit') {
      running = false;
      visible = false;
      closeMenu();
      paint();
      launcher.focus();
    }
  });
  document.addEventListener('pointerdown',event=>{
    if (!menu.hidden && !event.target.closest('#resident-menu,#resident-trigger')) closeMenu();
  });
  document.addEventListener('focusin',event=>{
    if (!menu.hidden && !event.target.closest('#resident-menu,#resident-trigger')) closeMenu();
  });
  host.querySelectorAll('[data-platform-choice]').forEach(button=>button.onclick=()=>{
    platform = button.dataset.platformChoice;
    closeMenu();
    paint();
  });
  document.querySelectorAll('[data-hide-launcher]').forEach(button=>button.onclick=hide);
  launcher.onclick=()=>{
    query.textContent='';
    query.dispatchEvent(new Event('input'));
    restore();
  };
  updateDialog.querySelector('[data-close]').onclick=()=>updateDialog.close();
  updateDialog.addEventListener('close',()=>trigger.focus());
  document.addEventListener('fulcrum-update',()=>{
    paint();
    if (updateDialog.open) updates.render(updateDialog.querySelector('.shared-update'));
  });
  window.FulcrumResident = {hide, settingsLink};
  paint();
  if (!visible) trigger.focus();
})();
