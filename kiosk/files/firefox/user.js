// Апаратне прискорення для YouTube та відео (Intel Iris Xe)
user_pref("media.ffmpeg.vaapi.enabled", true);
user_pref("gfx.webrender.all", true);
user_pref("media.hardware-video-decoding.force-enabled", true);
user_pref("widget.dmabuf.force-enabled", true);

// Попереднє встановлення з'єднань при наведенні на посилання
user_pref("network.dns.disablePrefetch", false);

// Кеш: на диск (NVMe швидкий), у RAM лише невеликий гарячий шар
user_pref("browser.cache.disk.enable", true);
user_pref("browser.cache.disk.smart_size.enabled", false);
user_pref("browser.cache.disk.capacity", 1048576);       // 1 ГБ на диску
user_pref("browser.cache.memory.enable", true);
user_pref("browser.cache.memory.capacity", 65536);       // 64 МБ у RAM (було 512)
user_pref("browser.cache.memory.max_entry_size", 5120);  // 5 МБ на файл

// Вивантаження вкладок, коли RAM закінчується (Linux рахує у % вільної пам'яті)
user_pref("browser.tabs.unloadOnLowMemory", true);
user_pref("browser.low_commit_space_threshold_percent", 20); // <20% вільної → спати старі вкладки
user_pref("browser.tabs.min_inactive_duration_before_unload", 300000); // не чіпати вкладки, активні за 5 хв

// Менше сторінок «назад» тримаємо цілими в пам'яті (bfcache)
user_pref("browser.sessionhistory.max_total_viewers", 2);
user_pref("browser.sessionhistory.max_entries", 25);

// Декодовані картинки: стеля кешу й швидше скидання невидимих
user_pref("image.mem.surfacecache.max_size_kb", 262144); // 256 МБ
user_pref("image.mem.surfacecache.min_expiration_ms", 30000);

// Не тримати прогрітий процес про запас
user_pref("dom.ipc.processPrelaunch.enabled", false);
user_pref("browser.tabs.remote.warmup.enabled", false);

// Без префетчу сторінок, які, можливо, й не відкриєш
user_pref("network.prefetch-next", false);
user_pref("network.http.speculative-parallel-limit", 4);

// ── Ультраплавний скролінг (SmoothScroll / Smooth kinetic scrolling) ──
user_pref("general.smoothScroll", true);
user_pref("general.smoothScroll.lines.durationMinMS", 125);
user_pref("general.smoothScroll.lines.durationMaxMS", 200);
user_pref("general.smoothScroll.pages.durationMinMS", 125);
user_pref("general.smoothScroll.pages.durationMaxMS", 200);
user_pref("general.smoothScroll.mouseWheel.durationMinMS", 150);
user_pref("general.smoothScroll.mouseWheel.durationMaxMS", 300);
user_pref("general.smoothScroll.currentVelocityWeighting", "0.15");
user_pref("general.smoothScroll.stopDecelerationWeighting", "0.6");
user_pref("apz.overscroll.enabled", true);
user_pref("toolkit.scrollbox.smoothScroll", true);
