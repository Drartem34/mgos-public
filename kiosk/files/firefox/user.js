// Апаратне прискорення для YouTube та відео (Intel Iris Xe)
user_pref("media.ffmpeg.vaapi.enabled", true);
user_pref("gfx.webrender.all", true);
user_pref("media.hardware-video-decoding.force-enabled", true);
user_pref("widget.dmabuf.force-enabled", true);

// Попереднє встановлення з'єднань при наведенні на посилання
user_pref("network.http.speculative-parallel-limit", 10);
user_pref("network.dns.disablePrefetch", false);
user_pref("network.prefetch-next", true);

// Кешування в оперативну пам'ять (RAM Cache замість диску)
user_pref("browser.cache.disk.enable", false);
user_pref("browser.cache.memory.enable", true);
user_pref("browser.cache.memory.capacity", 524288); // 512 МБ під ультрашвидкий RAM кеш
user_pref("browser.cache.memory.max_entry_size", 51200); // Ліміт розміру одного файлу в RAM (50 МБ)

// Автоматичний сон для фонових неактивних вкладок
user_pref("browser.tabs.unloadOnLowMemory", true);
user_pref("browser.tabs.min_inactive_duration_before_unload", 600000); // 10 хвилин
user_pref("browser.low_commit_space_threshold_mb", 3072);

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
