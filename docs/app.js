/**
 * PStream - Ultra-Fidelity PlayStation 5 Application Engine
 * Native HLS Video Pipeline - DualSense Controller Engine - Zero Ads Guarantee
 * Version 1.0 Release Baseline Overhaul
 */
var App = (function () {
    "use strict";

    function log(msg) {
        console.log("[PStream PS5]", msg);
        try {
            fetch("/log?msg=" + encodeURIComponent(msg));
        } catch (e) { }
    }

    var STORAGE_KEY = "PSTREAM_CONTINUE_WATCHING";
    var FAVORITES_KEY = "PSTREAM_FAVORITES";
    var PROVIDER_KEY = "pstream_provider";
    var SKIP_KEY = "pstream_skip";
    var AUTOPLAY_KEY = "pstream_autoplay";
    var SUBSIZE_KEY = "pstream_subsize";

    var state = {
        view: "browse", // "browse" | "detail" | "player" | "context" | "drawer_episodes" | "drawer_audio" | "provider_modal" | "settings_modal"
        currentTab: "home",
        activeProvider: "eng",
        skipStep: 10,
        autoplay: true,
        subSize: "large",
        currentItem: null,
        currentDetails: null,
        currentSeason: 1,
        currentEpisode: 1,
        currentEpTitle: "",
        catalogFeed: null,
        continueList: [],
        favoritesList: [],
        isPaused: false,
        currentTime: 0,
        duration: 0,
        savedResumeTime: 0,
        aspectMode: "contain", // "contain" | "cover" | "fill"
        autoplayDismissed: false,
        inSearch: false
    };

    // TV Spatial Navigation Coordinates
    var nav = {
        tier: "billboard", // "tabs" | "search" | "billboard" | "continue" | "shelves"
        shelfIndex: 0,
        cardIndex: 0,
        billboardBtnIndex: 0,
        tabIndex: 0,
        focusedElement: null
    };

    var modalNav = {
        section: "play", // "close" | "play" | "seasons" | "episodes" | "recs"
        seasonIndex: 0,
        episodeIndex: 0,
        recIndex: 0
    };

    var playerNav = {
        tier: "center", // "top" | "center" | "scrubber" | "bottom"
        centerIndex: 1, // 0: Rewind 10s, 1: Play/Pause, 2: Forward 10s
        bottomIndex: 1  // 0: Aspect, 1: Prev Ep, 2: Episodes, 3: Audio, 4: Next Ep
    };

    var contextNav = {
        itemIndex: 0,
        targetCard: null
    };

    var playerContainer = null;
    var playerOverlay = null;
    var playerVideo = null;
    var detailModal = null;
    var osdHideTimeout = null;
    var hlsInstance = null;
    var lastProgressSaveTime = 0;

    function init() {
        playerContainer = document.getElementById("player-container");
        playerOverlay = document.getElementById("player-overlay");
        playerVideo = document.getElementById("player-video");
        detailModal = document.getElementById("detail-modal");

        // Load Settings
        var validProviders = ["eng", "ger", "ita", "esp", "fra"];
        var savedProv = localStorage.getItem(PROVIDER_KEY);
        if (savedProv && validProviders.indexOf(savedProv) !== -1) {
            state.activeProvider = savedProv;
        } else {
            state.activeProvider = "eng";
            savedProv = null;
        }

        var savedSkip = parseInt(localStorage.getItem(SKIP_KEY), 10);
        state.skipStep = (savedSkip === 10 || savedSkip === 15 || savedSkip === 30) ? savedSkip : 10;

        var savedAutoplay = localStorage.getItem(AUTOPLAY_KEY);
        state.autoplay = (savedAutoplay !== "off");

        var savedSubSize = localStorage.getItem(SUBSIZE_KEY);
        state.subSize = (savedSubSize === "normal" || savedSubSize === "large" || savedSubSize === "xlarge") ? savedSubSize : "large";
        updateSubtitleStyle();
        updateSeekButtonLabels();

        // Load Persistence
        state.continueList = loadContinueWatching();
        state.favoritesList = loadFavorites();

        setupVideoEventListeners();
        setupDomEventListeners();
        loadTabFeed("home");

        // Initialize PlayStation Controller Engine
        if (window.Input) {
            Input.init();
            Input.onKey(handleKey);
        }

        // Focus billboard play button on startup
        setTimeout(function () {
            focusBillboardPlay();
        }, 300);

        log("PStream PlayStation 5 Engine Initialized (v1.0 Baseline)");
    }

    // ==========================================
    // Video Player & HLS Stream Engine
    // ==========================================

    function setupVideoEventListeners() {
        if (!playerVideo) return;

        playerVideo.addEventListener("loadedmetadata", function () {
            state.duration = playerVideo.duration || 0;
            log("Video metadata loaded. Duration: " + state.duration);
            updateTimelineUI();
            hideSpinner();

            // Resume from saved timestamp
            if (state.savedResumeTime && state.savedResumeTime > 10) {
                var seekTo = state.savedResumeTime;
                state.savedResumeTime = 0;
                if (!state.duration || seekTo < state.duration - 25) {
                    playerVideo.currentTime = seekTo;
                    state.currentTime = seekTo;
                    log("Resumed playback at saved timestamp: " + formatTime(seekTo));
                }
            }
        });

        playerVideo.addEventListener("timeupdate", function () {
            state.currentTime = playerVideo.currentTime || 0;
            if (!state.duration || state.duration === 0) {
                state.duration = playerVideo.duration || 0;
            }
            updateTimelineUI();
            checkSeriesAutoplay();

            // Throttle progress persistence to storage every 3 seconds
            var now = Date.now();
            if (now - lastProgressSaveTime > 3000) {
                lastProgressSaveTime = now;
                updatePlaybackProgress(state.currentItem, state.currentTime, state.duration);
            }
        });

        playerVideo.addEventListener("play", function () {
            state.isPaused = false;
            updatePlayPauseUI();
            hideSpinner();
        });

        playerVideo.addEventListener("pause", function () {
            state.isPaused = true;
            updatePlayPauseUI();
            updatePlaybackProgress(state.currentItem, state.currentTime, state.duration);
        });

        playerVideo.addEventListener("waiting", function () {
            showSpinner("Buffering High-Speed Stream...");
        });

        playerVideo.addEventListener("playing", function () {
            state.isPaused = false;
            updatePlayPauseUI();
            hideSpinner();
        });

        playerVideo.addEventListener("canplay", function () {
            hideSpinner();
        });

        playerVideo.addEventListener("ended", function () {
            updatePlaybackProgress(state.currentItem, 0, state.duration);
            if (state.autoplay && state.currentItem && state.currentItem.type === "tv") {
                triggerAutoplayNextNow();
            } else {
                wakePlayerOsd();
            }
        });
    }

    function playMedia(item, season, episode, epTitle) {
        state.currentItem = item;
        state.currentSeason = season || 1;
        state.currentEpisode = episode || 1;
        state.currentEpTitle = epTitle || "";
        state.view = "player";
        state.isPaused = false;
        state.currentTime = 0;
        state.duration = 0;
        state.autoplayDismissed = false;

        try {
            if (document.activeElement && document.activeElement.blur) {
                document.activeElement.blur();
            }
        } catch (e) { }

        document.body.classList.add("in-player");

        // Check if there is saved continue watching progress to resume
        var savedEntry = findContinueItem(item.id);
        if (savedEntry && savedEntry.currentTime && savedEntry.currentTime > 10) {
            if (item.type === "tv") {
                if (savedEntry.season === state.currentSeason && savedEntry.episode === state.currentEpisode) {
                    state.savedResumeTime = savedEntry.currentTime;
                }
            } else {
                state.savedResumeTime = savedEntry.currentTime;
            }
        }

        var isTv = (item.type === "tv");

        // Format Title centered in Top Row
        var titleHeader = document.getElementById("player-header-title");
        if (titleHeader) {
            if (isTv) {
                var epStr = 'S' + state.currentSeason + ':E' + state.currentEpisode;
                if (epTitle) {
                    epStr += ' "' + epTitle + '"';
                } else {
                    epStr += ' Episode ' + state.currentEpisode;
                }
                titleHeader.innerText = item.title + ' - ' + epStr;
            } else {
                titleHeader.innerText = item.title;
            }
        }

        // Configure Series Action Buttons in Bottom Row
        var prevEpBtn = document.getElementById("player-prev-ep-btn");
        var epDrawerBtn = document.getElementById("player-episodes-btn");
        var nextEpBtn = document.getElementById("player-next-ep-btn");

        if (isTv) {
            if (prevEpBtn) {
                prevEpBtn.style.display = (state.currentEpisode > 1) ? "inline-flex" : "none";
            }
            if (epDrawerBtn) epDrawerBtn.style.display = "inline-flex";
            if (nextEpBtn) nextEpBtn.style.display = "inline-flex";
        } else {
            if (prevEpBtn) prevEpBtn.style.display = "none";
            if (epDrawerBtn) epDrawerBtn.style.display = "none";
            if (nextEpBtn) nextEpBtn.style.display = "none";
        }

        // Show player container
        playerContainer.style.display = "block";
        hideErrorModal();

        // Save progress to Continue Watching
        saveContinueWatching(item, state.currentSeason, state.currentEpisode, epTitle, state.savedResumeTime || 0, 0);

        // Show spinner & reset UI
        showSpinner("Connecting to Ad-Free Stream...", item.title);
        updatePlayPauseUI();
        updateTimelineUI();

        // Focus Big Center Play/Pause button
        playerNav.tier = "center";
        playerNav.centerIndex = 1;
        wakePlayerOsd();
        var centerPlay = document.getElementById("player-center-playpause");
        if (centerPlay) setFocus(centerPlay);

        // Request stream resolution (Cloud Edge Proxy / Zero-PC Standalone)
        var cloudProxyPrefix = "https://pstream-proxy.ducky-ux1.workers.dev";
        var isLocalDev = (window.location.protocol === "http:" && (window.location.port === "8080" || window.location.hostname === "127.0.0.1" || window.location.hostname === "localhost"));
        var apiBase = isLocalDev ? "" : cloudProxyPrefix;

        var resolveQuery = "id=" + encodeURIComponent(item.id) +
            "&type=" + encodeURIComponent(item.type || "movie") +
            "&season=" + encodeURIComponent(state.currentSeason) +
            "&episode=" + encodeURIComponent(state.currentEpisode) +
            "&provider=" + encodeURIComponent(state.activeProvider) +
            "&lang=" + encodeURIComponent(state.activeProvider);

        var resolveUrl = (apiBase ? apiBase : "") + "/api/stream/resolve?" + resolveQuery;

        log("Resolving direct stream: " + resolveUrl);

        fetch(resolveUrl)
            .then(function (res) { return res.json(); })
            .then(function (data) {
                if (data.status === "ok" && data.streamUrl) {
                    loadHlsStream(data.streamUrl);
                } else {
                    hideSpinner();
                    showErrorModal(data.error || "Ad-free stream unavailable from this provider. Press X to retry or switch provider in Settings.");
                }
            })
            .catch(function (err) {
                log("Stream resolution error: " + err);
                hideSpinner();
                showErrorModal("Connecting to streaming mirror. Press X to retry, or select another provider in Settings.");
            });
    }

    function loadHlsStream(streamUrl) {
        log("Loading HLS stream into video player: " + streamUrl);

        if (hlsInstance) {
            hlsInstance.destroy();
            hlsInstance = null;
        }

        if (window.Hls && Hls.isSupported()) {
            hlsInstance = new Hls({
                enableWorker: false,
                maxBufferSize: 20 * 1024 * 1024,
                maxBufferLength: 20,
                maxMaxBufferLength: 35,
                backBufferLength: 10,
                lowLatencyMode: false
            });

            hlsInstance.loadSource(streamUrl);
            hlsInstance.attachMedia(playerVideo);

            hlsInstance.on(Hls.Events.MANIFEST_PARSED, function () {
                log("HLS Manifest Parsed! Launching native playback...");
                playerVideo.play().catch(function (e) {
                    log("Auto-play blocked, awaiting user input: " + e);
                });
            });

            hlsInstance.on(Hls.Events.ERROR, function (event, data) {
                log("Hls error: " + data.type + " " + data.details);
                if (data.fatal) {
                    if (data.type === Hls.ErrorTypes.NETWORK_ERROR) {
                        hlsInstance.startLoad();
                    } else if (data.type === Hls.ErrorTypes.MEDIA_ERROR) {
                        hlsInstance.recoverMediaError();
                    } else {
                        hlsInstance.destroy();
                        hlsInstance = null;
                        playerVideo.src = streamUrl;
                        playerVideo.play().catch(function () {});
                    }
                }
            });
        } else if (playerVideo.canPlayType('application/vnd.apple.mpegurl')) {
            log("Using native WebKit HLS playback");
            playerVideo.src = streamUrl;
            playerVideo.play().then(function () {
                hideSpinner();
            }).catch(function (e) {
                log("Native play error: " + e);
                hideSpinner();
            });
        } else {
            playerVideo.src = streamUrl;
            playerVideo.play().catch(function () {});
            hideSpinner();
        }
    }

    function togglePlayPause() {
        if (!playerVideo) return;
        if (playerVideo.paused) {
            playerVideo.play().catch(function () {});
        } else {
            playerVideo.pause();
        }
        updatePlayPauseUI();
    }

    function seekRelative(seconds) {
        if (!playerVideo) return;
        var cur = playerVideo.currentTime || 0;
        var dur = playerVideo.duration || state.duration || 0;
        var target = Math.max(0, Math.min(dur, cur + seconds));
        playerVideo.currentTime = target;
        state.currentTime = target;
        updateTimelineUI();
        updatePlaybackProgress(state.currentItem, target, dur);
    }

    function playNextEpisode() {
        if (!state.currentItem || state.currentItem.type !== "tv") return;
        var nextEp = state.currentEpisode + 1;
        log("Advancing to next episode: S" + state.currentSeason + " E" + nextEp);
        playMedia(state.currentItem, state.currentSeason, nextEp);
    }

    function playPreviousEpisode() {
        if (!state.currentItem || state.currentItem.type !== "tv" || state.currentEpisode <= 1) return;
        var prevEp = state.currentEpisode - 1;
        log("Returning to previous episode: S" + state.currentSeason + " E" + prevEp);
        playMedia(state.currentItem, state.currentSeason, prevEp);
    }

    function toggleAspectRatio() {
        if (!playerVideo) return;
        var modes = ["contain", "cover", "fill"];
        var curr = modes.indexOf(state.aspectMode);
        var next = modes[(curr + 1) % modes.length];
        state.aspectMode = next;
        playerVideo.style.objectFit = next;
    }

    function exitPlayer() {
        if (playerVideo) {
            updatePlaybackProgress(state.currentItem, playerVideo.currentTime, playerVideo.duration);
            playerVideo.pause();
            playerVideo.removeAttribute("src");
        }
        if (hlsInstance) {
            hlsInstance.destroy();
            hlsInstance = null;
        }
        document.body.classList.remove("in-player");
        playerContainer.style.display = "none";
        hideSpinner();
        hideErrorModal();
        closeEpisodesDrawer();
        closeAudioDrawer();
        state.view = "browse";
        var searchInput = document.getElementById("search-input");
        if (searchInput && searchInput.value) {
            searchInput.value = "";
            updateSearchClearBtn();
        }
        if (state.inSearch) {
            state.inSearch = false;
            loadTabFeed(state.currentTab || "home");
        } else {
            renderContinueWatching();
            focusBillboardPlay();
        }
    }

    // ==========================================
    // Player UI & OSD Updates
    // ==========================================

    function updatePlayPauseUI() {
        var iconEl = document.getElementById("center-playpause-icon");
        if (!iconEl) return;
        if (state.isPaused) {
            // Show large Play triangle
            iconEl.innerHTML = '<polygon points="20,12 50,30 20,48" fill="#ffffff"/>';
        } else {
            // Show large Pause bars
            iconEl.innerHTML = '<rect x="18" y="14" width="8" height="32" rx="2" fill="#ffffff"/><rect x="34" y="14" width="8" height="32" rx="2" fill="#ffffff"/>';
        }
    }

    function updateTimelineUI() {
        var fill = document.getElementById("player-scrubber-fill");
        var knob = document.getElementById("player-scrubber-knob");
        var remainingEl = document.getElementById("player-scrubber-remaining");
        var buffered = document.getElementById("player-scrubber-buffered");

        var cur = state.currentTime;
        var dur = state.duration;

        if (dur > 0) {
            var pct = Math.min(100, (cur / dur) * 100);
            if (fill) fill.style.width = pct + "%";
            if (knob) knob.style.left = pct + "%";

            var rem = Math.max(0, dur - cur);
            if (remainingEl) remainingEl.innerText = formatTime(rem);

            if (playerVideo && playerVideo.buffered && playerVideo.buffered.length > 0 && buffered) {
                var bufEnd = playerVideo.buffered.end(playerVideo.buffered.length - 1);
                var bufPct = Math.min(100, (bufEnd / dur) * 100);
                buffered.style.width = bufPct + "%";
            }
        } else {
            if (fill) fill.style.width = "0%";
            if (knob) knob.style.left = "0%";
            if (remainingEl) remainingEl.innerText = "0:00";
        }
    }

    function formatTime(sec) {
        if (!sec || isNaN(sec) || sec <= 0) return "0:00";
        sec = Math.floor(sec);
        var h = Math.floor(sec / 3600);
        var m = Math.floor((sec % 3600) / 60);
        var s = sec % 60;

        if (h > 0) {
            return h + ":" + (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s;
        }
        return m + ":" + (s < 10 ? "0" : "") + s;
    }

    function resetPlayerFocusToPlay() {
        playerNav.tier = "center";
        playerNav.centerIndex = 1;
        var centerPlay = document.getElementById("player-center-playpause");
        if (centerPlay) setFocus(centerPlay);
    }

    function wakePlayerOsd() {
        if (!playerOverlay) return;
        var wasHidden = playerOverlay.classList.contains("osd-hidden");
        playerOverlay.classList.remove("osd-hidden");

        if (wasHidden) {
            resetPlayerFocusToPlay();
        }

        clearTimeout(osdHideTimeout);
        osdHideTimeout = setTimeout(function () {
            if (state.view === "player" && (!playerVideo || !playerVideo.paused)) {
                playerOverlay.classList.add("osd-hidden");
                resetPlayerFocusToPlay();
            }
        }, 4500);
    }

    function showSpinner(statusText, titleText) {
        var wrap = document.getElementById("player-spinner-wrap");
        var title = document.getElementById("spinner-title");
        var stat = document.getElementById("spinner-status");
        if (!wrap) return;
        if (title && state.currentItem) {
            title.innerText = titleText || state.currentItem.title;
        }
        if (stat && statusText) {
            stat.innerText = statusText;
        }
        wrap.classList.add("active");
    }

    function hideSpinner() {
        var wrap = document.getElementById("player-spinner-wrap");
        if (wrap) wrap.classList.remove("active");
    }

    function showErrorModal(msg) {
        var modal = document.getElementById("player-error-modal");
        var text = document.getElementById("player-error-message");
        if (!modal) return;
        if (text) text.innerText = msg;
        modal.style.display = "flex";
        var retry = document.getElementById("player-error-retry");
        if (retry) setFocus(retry);
    }

    function hideErrorModal() {
        var modal = document.getElementById("player-error-modal");
        if (modal) modal.style.display = "none";
    }

    // Series Autoplay Countdown
    var autoplayCountdownTimer = null;
    var autoplaySecondsLeft = 5;

    function checkSeriesAutoplay() {
        if (!state.autoplay) return;
        if (!state.currentItem || state.currentItem.type !== "tv") return;
        if (!state.duration || state.duration <= 30) return;
        var rem = state.duration - state.currentTime;
        if (rem <= 25 && rem > 0 && !autoplayCountdownTimer && !state.autoplayDismissed) {
            showAutoplayNextCard();
        }
    }

    function showAutoplayNextCard() {
        var card = document.getElementById("autoplay-card");
        var titleEl = document.getElementById("autoplay-title");
        var timerEl = document.getElementById("autoplay-timer");
        if (!card) return;

        var nextEpNum = state.currentEpisode + 1;
        if (titleEl) titleEl.innerText = "Season " + state.currentSeason + ", Episode " + nextEpNum;
        autoplaySecondsLeft = 5;
        if (timerEl) timerEl.innerText = autoplaySecondsLeft;
        card.style.display = "block";

        clearInterval(autoplayCountdownTimer);
        autoplayCountdownTimer = setInterval(function () {
            autoplaySecondsLeft--;
            if (timerEl) timerEl.innerText = autoplaySecondsLeft;
            if (autoplaySecondsLeft <= 0) {
                clearInterval(autoplayCountdownTimer);
                autoplayCountdownTimer = null;
                hideAutoplayCard();
                playNextEpisode();
            }
        }, 1000);
    }

    function hideAutoplayCard() {
        clearInterval(autoplayCountdownTimer);
        autoplayCountdownTimer = null;
        var card = document.getElementById("autoplay-card");
        if (card) card.style.display = "none";
    }

    function triggerAutoplayNextNow() {
        hideAutoplayCard();
        playNextEpisode();
    }

    // ==========================================
    // In-Player Drawers (Episodes & Audio/Subs)
    // ==========================================

    function toggleEpisodesDrawer() {
        var drawer = document.getElementById("player-episodes-drawer");
        if (!drawer) return;
        if (drawer.style.display === "flex") {
            closeEpisodesDrawer();
        } else {
            openEpisodesDrawer();
        }
    }

    function openEpisodesDrawer() {
        if (!state.currentItem || state.currentItem.type !== "tv") {
            return;
        }
        var drawer = document.getElementById("player-episodes-drawer");
        if (!drawer) return;
        drawer.style.display = "flex";
        state.view = "drawer_episodes";

        document.getElementById("drawer-series-title").innerText = state.currentItem.title;
        loadDrawerEpisodes(state.currentItem.id, state.currentSeason);
    }

    function closeEpisodesDrawer() {
        var drawer = document.getElementById("player-episodes-drawer");
        if (drawer) drawer.style.display = "none";
        if (state.view === "drawer_episodes") {
            state.view = "player";
            var epBtn = document.getElementById("player-episodes-btn");
            if (epBtn) setFocus(epBtn);
        }
    }

    function loadDrawerEpisodes(tvId, seasonNum) {
        var container = document.getElementById("drawer-episodes-list");
        container.innerHTML = '<div style="color:#888;padding:20px;">Loading episodes...</div>';

        // Load season pills directly from TMDb
        function renderPills(seasons) {
            var pillsWrap = document.getElementById("drawer-season-pills");
            pillsWrap.innerHTML = "";
            seasons.forEach(function (s) {
                var pill = document.createElement("button");
                pill.className = "season-pill" + (s.season_number === seasonNum ? " active" : "");
                pill.innerText = s.name || ("Season " + s.season_number);
                pill.onclick = function () {
                    document.querySelectorAll("#drawer-season-pills .season-pill").forEach(function (p) { p.classList.remove("active"); });
                    pill.classList.add("active");
                    loadDrawerEpisodes(tvId, s.season_number);
                };
                pillsWrap.appendChild(pill);
            });
        }

        TMDb.getDetails(tvId, "tv", state.activeProvider)
            .then(function (det) {
                renderPills(det.seasons || []);
            })
            .catch(function () {
                fetch("/api/tv?id=" + tvId + "&lang=" + encodeURIComponent(state.activeProvider || "eng"))
                    .then(function (r) { return r.json(); })
                    .then(function (data) {
                        renderPills(data.seasons || []);
                    })
                    .catch(function () {});
            });

        function renderDrawerEps(eps) {
            container.innerHTML = "";
            if (eps.length === 0) {
                container.innerHTML = '<div style="color:#888;">No episodes found.</div>';
                return;
            }

            eps.forEach(function (ep) {
                var item = document.createElement("div");
                item.className = "episode-item";
                var isCurrent = (ep.episode_number === state.currentEpisode && seasonNum === state.currentSeason);
                if (isCurrent) {
                    item.style.borderColor = "var(--red-pstream)";
                }

                var thumb = ep.still || (state.currentItem ? state.currentItem.backdrop : "");
                item.innerHTML =
                    '<div class="episode-thumb-wrap">' +
                        '<img src="' + thumb + '" alt="' + ep.name + '">' +
                    '</div>' +
                    '<div class="episode-meta-col">' +
                        '<div class="episode-title-row">' +
                            '<span>' + ep.episode_number + '. ' + ep.name + '</span>' +
                            '<span class="episode-runtime">' + (ep.runtime || "") + '</span>' +
                        '</div>' +
                        '<div class="episode-synopsis">' + (ep.overview || "No overview available.") + '</div>' +
                    '</div>';

                item.onclick = function () {
                    closeEpisodesDrawer();
                    playMedia(state.currentItem, seasonNum, ep.episode_number, ep.name);
                };

                container.appendChild(item);
            });

            var firstEp = container.querySelector(".episode-item");
            if (firstEp) setFocus(firstEp);
        }

        TMDb.getSeason(tvId, seasonNum, state.activeProvider)
            .then(function (data) {
                renderDrawerEps(data.episodes || []);
            })
            .catch(function () {
                fetch("/api/season?id=" + tvId + "&season=" + seasonNum + "&lang=" + encodeURIComponent(state.activeProvider || "eng"))
                    .then(function (r) { return r.json(); })
                    .then(function (data) {
                        renderDrawerEps(data.episodes || []);
                    })
                    .catch(function () {});
            });
    }

    function toggleAudioDrawer() {
        var drawer = document.getElementById("player-audio-drawer");
        if (!drawer) return;
        if (drawer.style.display === "block") {
            closeAudioDrawer();
        } else {
            openAudioDrawer();
        }
    }

    function openAudioDrawer() {
        var drawer = document.getElementById("player-audio-drawer");
        if (!drawer) return;
        drawer.style.display = "block";
        state.view = "drawer_audio";

        var audioList = document.getElementById("audio-tracks-list");
        var subList = document.getElementById("subtitles-list");
        audioList.innerHTML = "";
        subList.innerHTML = "";

        if (hlsInstance && hlsInstance.audioTracks && hlsInstance.audioTracks.length > 0) {
            hlsInstance.audioTracks.forEach(function (track, idx) {
                var btn = document.createElement("button");
                btn.className = "track-item" + (idx === hlsInstance.audioTrack ? " active" : "");
                btn.innerText = track.name || track.lang || ("Audio Track " + (idx + 1));
                btn.onclick = function () {
                    hlsInstance.audioTrack = idx;
                    closeAudioDrawer();
                };
                audioList.appendChild(btn);
            });
        } else {
            audioList.innerHTML = '<div class="track-item active">English [Original Stereo]</div>';
        }

        var subOff = document.createElement("button");
        subOff.className = "track-item active";
        subOff.innerText = "Subtitles Off";
        subOff.onclick = function () {
            if (hlsInstance) hlsInstance.subtitleTrack = -1;
            closeAudioDrawer();
        };
        subList.appendChild(subOff);

        if (hlsInstance && hlsInstance.subtitleTracks && hlsInstance.subtitleTracks.length > 0) {
            hlsInstance.subtitleTracks.forEach(function (track, idx) {
                var subBtn = document.createElement("button");
                subBtn.className = "track-item";
                subBtn.innerText = track.name || track.lang || ("Subtitle " + (idx + 1));
                subBtn.onclick = function () {
                    hlsInstance.subtitleTrack = idx;
                    closeAudioDrawer();
                };
                subList.appendChild(subBtn);
            });
        }

        var firstTrack = audioList.querySelector(".track-item");
        if (firstTrack) setFocus(firstTrack);
    }

    function closeAudioDrawer() {
        var drawer = document.getElementById("player-audio-drawer");
        if (drawer) drawer.style.display = "none";
        if (state.view === "drawer_audio") {
            state.view = "player";
            var audioBtn = document.getElementById("player-audio-sub-btn");
            if (audioBtn) setFocus(audioBtn);
        }
    }

    // ==========================================
    // PlayStation Card Context Menu (Options Button)
    // ==========================================

    function openCardContextMenu(targetCard) {
        if (!targetCard) return;
        var menu = document.getElementById("card-context-menu");
        var titleEl = document.getElementById("context-menu-title");
        var favLabel = document.getElementById("ctx-fav-label");
        var removeContBtn = document.getElementById("ctx-remove-continue");

        contextNav.targetCard = targetCard;
        var mediaId = targetCard.dataset.id;

        var cardTitle = targetCard.querySelector(".card-title-text, .card-title, h3");
        var titleText = cardTitle ? cardTitle.innerText : "Title";
        titleEl.innerText = titleText;

        // Is in Continue Watching?
        var isContinue = targetCard.closest("#row-continue") || findContinueItem(mediaId);
        removeContBtn.style.display = isContinue ? "flex" : "none";

        // Is in Favorites?
        var isFav = isFavorited(mediaId);
        favLabel.innerText = isFav ? "Remove from My List / Favorites" : "Add to My List / Favorites";

        menu.style.display = "flex";
        state.view = "context";

        var firstOption = menu.querySelector(".context-menu-item:not([style*='display: none'])");
        if (firstOption) setFocus(firstOption);
    }

    function closeCardContextMenu() {
        var menu = document.getElementById("card-context-menu");
        if (menu) menu.style.display = "none";
        if (state.view === "context") {
            state.view = "browse";
            if (contextNav.targetCard) setFocus(contextNav.targetCard);
        }
    }

    function executeContextAction(action) {
        var card = contextNav.targetCard;
        if (!card) {
            closeCardContextMenu();
            return;
        }

        var mid = card.dataset.id;
        var mtype = card.dataset.type || "movie";

        if (action === "remove-continue") {
            removeContinueWatching(mid);
            closeCardContextMenu();
        } else if (action === "toggle-favorite") {
            toggleFavorite({
                id: mid,
                type: mtype,
                title: card.querySelector(".card-title-text") ? card.querySelector(".card-title-text").innerText : "Title",
                poster: card.querySelector("img") ? card.querySelector("img").src : ""
            });
            closeCardContextMenu();
        } else if (action === "details") {
            closeCardContextMenu();
            openDetailModal(mid, mtype);
        } else if (action === "play") {
            closeCardContextMenu();
            var saved = findContinueItem(mid);
            if (saved) {
                playMedia(saved, saved.season || 1, saved.episode || 1, saved.epTitle || "");
            } else {
                openDetailModal(mid, mtype);
            }
        } else if (action === "close") {
            closeCardContextMenu();
        }
    }

    // ==========================================
    // Provider & Settings Modals
    // ==========================================

    function openProviderWelcomeModal() {
        var modal = document.getElementById("provider-welcome-modal");
        if (!modal) return;
        modal.style.display = "flex";
        state.view = "provider_modal";
        var firstBtn = modal.querySelector(".provider-option-btn");
        if (firstBtn) setFocus(firstBtn);
    }

    function closeProviderWelcomeModal() {
        var modal = document.getElementById("provider-welcome-modal");
        if (modal) modal.style.display = "none";
        if (state.view === "provider_modal") {
            state.view = "browse";
            focusBillboardPlay();
        }
    }

    function selectProvider(provId) {
        state.activeProvider = provId;
        localStorage.setItem(PROVIDER_KEY, provId);
        updateSettingsPills();
        closeProviderWelcomeModal();
        closeSettingsModal();
        log("Provider switched to: " + provId);
        loadTabFeed(state.currentTab || "home");
    }

    function setSkipStep(sec) {
        state.skipStep = sec;
        localStorage.setItem(SKIP_KEY, sec);
        updateSettingsPills();
        updateSeekButtonLabels();
        log("Skip step set to: " + sec + "s");
    }

    function setAutoplay(val) {
        state.autoplay = (val === "on");
        localStorage.setItem(AUTOPLAY_KEY, val);
        updateSettingsPills();
        log("Autoplay set to: " + val);
    }

    function setSubSize(size) {
        state.subSize = size;
        localStorage.setItem(SUBSIZE_KEY, size);
        updateSettingsPills();
        updateSubtitleStyle();
        log("Subtitle size set to: " + size);
    }

    function updateSubtitleStyle() {
        var styleEl = document.getElementById("pstream-sub-style");
        if (!styleEl) {
            styleEl = document.createElement("style");
            styleEl.id = "pstream-sub-style";
            document.head.appendChild(styleEl);
        }
        var sizeMap = {
            "normal": "20px",
            "large": "26px",
            "xlarge": "32px"
        };
        var fSize = sizeMap[state.subSize] || "26px";
        styleEl.textContent = "video::cue { font-size: " + fSize + " !important; background: rgba(0,0,0,0.75) !important; color: #ffffff !important; }";
    }

    function updateSeekButtonLabels() {
        var secStr = String(state.skipStep || 10);
        var rewTxt = document.querySelector("#player-rewind-10 text");
        if (rewTxt) rewTxt.textContent = secStr;
        var fwdTxt = document.querySelector("#player-forward-10 text");
        if (fwdTxt) fwdTxt.textContent = secStr;
    }

    function resetAllSettings() {
        localStorage.removeItem(PROVIDER_KEY);
        localStorage.removeItem(SKIP_KEY);
        localStorage.removeItem(AUTOPLAY_KEY);
        localStorage.removeItem(SUBSIZE_KEY);
        state.activeProvider = "eng";
        state.skipStep = 10;
        state.autoplay = true;
        state.subSize = "large";
        updateSubtitleStyle();
        updateSeekButtonLabels();
        updateSettingsPills();
        loadTabFeed(state.currentTab || "home");
        closeSettingsModal();
    }

    function openSettingsModal() {
        var modal = document.getElementById("settings-modal");
        if (!modal) return;
        modal.style.display = "flex";
        state.view = "settings_modal";
        updateSettingsPills();
        var firstPill = modal.querySelector(".setting-pill.active") || modal.querySelector(".setting-pill");
        if (firstPill) setFocus(firstPill);
    }

    function closeSettingsModal() {
        var modal = document.getElementById("settings-modal");
        if (modal) modal.style.display = "none";
        if (state.view === "settings_modal") {
            state.view = "browse";
            focusBillboardPlay();
        }
    }

    function updateSettingsPills() {
        // Provider pills
        document.querySelectorAll("#settings-provider-pills .setting-pill").forEach(function (pill) {
            if (pill.dataset.provider === state.activeProvider) {
                pill.classList.add("active");
            } else {
                pill.classList.remove("active");
            }
        });

        // Skip pills
        document.querySelectorAll("#settings-skip-pills .setting-pill").forEach(function (pill) {
            if (parseInt(pill.dataset.skip, 10) === state.skipStep) {
                pill.classList.add("active");
            } else {
                pill.classList.remove("active");
            }
        });

        // Autoplay pills
        document.querySelectorAll("#settings-autoplay-pills .setting-pill").forEach(function (pill) {
            var val = (pill.dataset.autoplay === "on");
            if (val === state.autoplay) {
                pill.classList.add("active");
            } else {
                pill.classList.remove("active");
            }
        });

        // Subtitle size pills
        document.querySelectorAll("#settings-subsize-pills .setting-pill").forEach(function (pill) {
            if (pill.dataset.subsize === state.subSize) {
                pill.classList.add("active");
            } else {
                pill.classList.remove("active");
            }
        });
    }

    // ==========================================
    // Persistence: Continue Watching & Favorites
    // ==========================================

    function loadContinueWatching() {
        try {
            var raw = localStorage.getItem(STORAGE_KEY);
            return raw ? JSON.parse(raw) : [];
        } catch (e) { return []; }
    }

    function saveContinueWatching(item, season, episode, epTitle, currentTime, duration) {
        try {
            var list = loadContinueWatching();
            list = list.filter(function (it) { return it.id !== item.id; });
            list.unshift({
                id: item.id,
                title: item.title,
                type: item.type || "movie",
                season: season || 1,
                episode: episode || 1,
                epTitle: epTitle || "",
                poster: item.poster || "",
                backdrop: item.backdrop || "",
                year: item.year || "",
                rating: item.rating || "",
                match: item.match || "98% Match",
                currentTime: Math.floor(currentTime || 0),
                duration: Math.floor(duration || 0),
                timestamp: Date.now()
            });
            if (list.length > 25) list = list.slice(0, 25);
            localStorage.setItem(STORAGE_KEY, JSON.stringify(list));
            state.continueList = list;
            renderContinueWatching();
        } catch (e) {
            log("Error saving continue watching: " + e);
        }
    }

    function updatePlaybackProgress(item, time, duration) {
        if (!item || !item.id) return;
        try {
            var list = loadContinueWatching();
            var entry = list.find(function (it) { return it.id === item.id; });
            if (entry) {
                entry.currentTime = Math.floor(time || 0);
                if (duration && duration > 0) entry.duration = Math.floor(duration);
                entry.timestamp = Date.now();
                localStorage.setItem(STORAGE_KEY, JSON.stringify(list));
                state.continueList = list;
            }
        } catch (e) { }
    }

    function removeContinueWatching(id) {
        try {
            var list = loadContinueWatching();
            list = list.filter(function (it) { return String(it.id) !== String(id); });
            localStorage.setItem(STORAGE_KEY, JSON.stringify(list));
            state.continueList = list;
            renderContinueWatching();
        } catch (e) {}
    }

    function findContinueItem(id) {
        return state.continueList.find(function (it) { return String(it.id) === String(id); });
    }

    function renderContinueWatching() {
        var sec = document.getElementById("continue-watching-section");
        var carousel = document.getElementById("row-continue");
        if (!sec || !carousel) return;

        var list = state.continueList;
        if (!list || list.length === 0) {
            sec.style.display = "none";
            return;
        }

        sec.style.display = "block";
        carousel.innerHTML = "";

        list.forEach(function (item) {
            var wrap = document.createElement("div");
            wrap.className = "netflix-card";
            wrap.dataset.id = item.id;
            wrap.dataset.type = item.type;

            var tag = (item.type === "tv") ? ("S" + item.season + ":E" + item.episode) : "MOVIE";
            var pct = 0;
            if (item.currentTime && item.duration) {
                pct = Math.min(100, Math.round((item.currentTime / item.duration) * 100));
            }
            var timeBadge = (item.currentTime > 0) ? (formatTime(item.currentTime) + (item.duration > 0 ? " / " + formatTime(item.duration) : "")) : "";

            wrap.innerHTML =
                '<img src="' + (item.poster || 'https://via.placeholder.com/500x750') + '" alt="' + item.title + '" loading="lazy">' +
                '<div class="card-badge-top-left">' + tag + '</div>' +
                (pct > 0 ? '<div class="card-progress-bar-wrap"><div class="card-progress-bar-fill" style="width:' + pct + '%;"></div></div>' : '') +
                (timeBadge ? '<div class="card-progress-timestamp">' + timeBadge + '</div>' : '') +
                '<div class="card-info-overlay">' +
                    '<div class="card-title-text">' + item.title + '</div>' +
                '</div>';

            wrap.addEventListener("click", function () {
                var saved = findContinueItem(item.id) || item;
                playMedia(saved, saved.season || 1, saved.episode || 1, saved.epTitle || "");
            });

            carousel.appendChild(wrap);
        });
    }

    function loadFavorites() {
        try {
            var raw = localStorage.getItem(FAVORITES_KEY);
            return raw ? JSON.parse(raw) : [];
        } catch (e) { return []; }
    }

    function isFavorited(id) {
        return state.favoritesList.some(function (f) { return String(f.id) === String(id); });
    }

    function toggleFavorite(item) {
        var list = loadFavorites();
        var exists = list.some(function (f) { return String(f.id) === String(item.id); });
        if (exists) {
            list = list.filter(function (f) { return String(f.id) !== String(item.id); });
        } else {
            list.unshift(item);
        }
        localStorage.setItem(FAVORITES_KEY, JSON.stringify(list));
        state.favoritesList = list;
    }

    // ==========================================
    // Standalone TMDb Client-Side Catalog Engine
    // 100% Zero-PC Required - Direct High-Speed CORS API
    // ==========================================
    var TMDb = (function () {
        var API_KEY = "adc5047f27e588c9347087931a696cf4";
        var BASE = "https://api.themoviedb.org/3";
        var cache = { feeds: {}, details: {}, seasons: {} };

        var LANG_MAP = {
            "eng": "en-US",
            "ita": "it-IT",
            "esp": "es-ES",
            "ger": "de-DE",
            "fra": "fr-FR",
            "anime": "ja-JP",
            "por": "pt-BR"
        };

        function getLang(prov) {
            return LANG_MAP[prov] || "en-US";
        }

        function tmdbGet(endpoint, lang) {
            var sep = endpoint.indexOf("?") !== -1 ? "&" : "?";
            var url = BASE + endpoint + sep + "api_key=" + API_KEY + "&language=" + (lang || "en-US");
            return fetch(url).then(function (res) {
                if (!res.ok) throw new Error("TMDb error " + res.status);
                return res.json();
            });
        }

        function formatItem(it, defType) {
            var mtype = it.media_type || defType || "movie";
            var title = it.title || it.name || it.original_title || "Unknown";
            var date = it.release_date || it.first_air_date || "";
            var year = date ? date.substring(0, 4) : "";
            var poster = it.poster_path ? ("https://image.tmdb.org/t/p/w500" + it.poster_path) : "";
            var backdrop = it.backdrop_path ? ("https://image.tmdb.org/t/p/w1280" + it.backdrop_path) : "";
            var rating = Math.round((it.vote_average || 0) * 10) / 10;
            var matchPct = rating > 0 ? Math.min(99, Math.max(75, Math.round(rating * 10 + 15))) : 92;

            return {
                id: String(it.id),
                title: title,
                type: mtype,
                year: year,
                overview: it.overview || "",
                rating: String(rating),
                match: matchPct + "% Match",
                poster: poster,
                backdrop: backdrop
            };
        }

        function getFeed(tab, provider) {
            var lang = getLang(provider);
            var cacheKey = tab + "_" + lang;
            if (cache.feeds[cacheKey]) {
                return Promise.resolve(cache.feeds[cacheKey]);
            }

            var feed = { tab: tab, featured: null, categories: [] };

            if (tab === "movies") {
                return Promise.all([
                    tmdbGet("/movie/popular", lang),
                    tmdbGet("/trending/movie/week", lang),
                    tmdbGet("/movie/top_rated", lang),
                    tmdbGet("/discover/movie?with_genres=28&sort_by=popularity.desc", lang),
                    tmdbGet("/discover/movie?with_genres=878&sort_by=popularity.desc", lang),
                    tmdbGet("/discover/movie?with_genres=27&sort_by=popularity.desc", lang),
                    tmdbGet("/discover/movie?with_genres=35&sort_by=popularity.desc", lang)
                ]).then(function (res) {
                    var pop = res[0], trend = res[1], top = res[2], act = res[3], scifi = res[4], horror = res[5], comedy = res[6];
                    if (trend && trend.results) {
                        var items = trend.results.filter(function (x) { return x.poster_path; }).map(function (x) { return formatItem(x, "movie"); });
                        if (items.length) {
                            feed.featured = items[0];
                            feed.categories.push({ title: "Top 10 Movies Today", isTop10: true, items: items.slice(0, 10) });
                        }
                    }
                    if (pop && pop.results) {
                        var pItems = pop.results.filter(function (x) { return x.poster_path; }).map(function (x) { return formatItem(x, "movie"); });
                        if (pItems.length) feed.categories.push({ title: "Popular on PStream", items: pItems });
                    }
                    if (top && top.results) {
                        var tItems = top.results.filter(function (x) { return x.poster_path; }).map(function (x) { return formatItem(x, "movie"); });
                        if (tItems.length) feed.categories.push({ title: "Critically Acclaimed Films", items: tItems });
                    }
                    if (act && act.results) {
                        var aItems = act.results.filter(function (x) { return x.poster_path; }).map(function (x) { return formatItem(x, "movie"); });
                        if (aItems.length) feed.categories.push({ title: "Action & Adventure", items: aItems });
                    }
                    if (scifi && scifi.results) {
                        var sItems = scifi.results.filter(function (x) { return x.poster_path; }).map(function (x) { return formatItem(x, "movie"); });
                        if (sItems.length) feed.categories.push({ title: "Sci-Fi & Fantasy", items: sItems });
                    }
                    if (horror && horror.results) {
                        var hItems = horror.results.filter(function (x) { return x.poster_path; }).map(function (x) { return formatItem(x, "movie"); });
                        if (hItems.length) feed.categories.push({ title: "Horror & Suspense", items: hItems });
                    }
                    if (comedy && comedy.results) {
                        var cItems = comedy.results.filter(function (x) { return x.poster_path; }).map(function (x) { return formatItem(x, "movie"); });
                        if (cItems.length) feed.categories.push({ title: "Comedy Hits", items: cItems });
                    }
                    cache.feeds[cacheKey] = feed;
                    return feed;
                });
            } else if (tab === "tv") {
                return Promise.all([
                    tmdbGet("/tv/popular", lang),
                    tmdbGet("/trending/tv/week", lang),
                    tmdbGet("/tv/top_rated", lang),
                    tmdbGet("/discover/tv?with_genres=18&sort_by=popularity.desc", lang),
                    tmdbGet("/discover/tv?with_genres=10765&sort_by=popularity.desc", lang),
                    tmdbGet("/discover/tv?with_genres=35&sort_by=popularity.desc", lang),
                    tmdbGet("/discover/tv?with_genres=80&sort_by=popularity.desc", lang)
                ]).then(function (res) {
                    var pop = res[0], trend = res[1], top = res[2], drama = res[3], scifi = res[4], comedy = res[5], crime = res[6];
                    if (trend && trend.results) {
                        var items = trend.results.filter(function (x) { return x.poster_path; }).map(function (x) { return formatItem(x, "tv"); });
                        if (items.length) {
                            feed.featured = items[0];
                            feed.categories.push({ title: "Top 10 Series Today", isTop10: true, items: items.slice(0, 10) });
                        }
                    }
                    if (pop && pop.results) {
                        var pItems = pop.results.filter(function (x) { return x.poster_path; }).map(function (x) { return formatItem(x, "tv"); });
                        if (pItems.length) feed.categories.push({ title: "Binge-Worthy TV Shows", items: pItems });
                    }
                    if (top && top.results) {
                        var tItems = top.results.filter(function (x) { return x.poster_path; }).map(function (x) { return formatItem(x, "tv"); });
                        if (tItems.length) feed.categories.push({ title: "All-Time Great Television", items: tItems });
                    }
                    if (drama && drama.results) {
                        var dItems = drama.results.filter(function (x) { return x.poster_path; }).map(function (x) { return formatItem(x, "tv"); });
                        if (dItems.length) feed.categories.push({ title: "Prestige Dramas", items: dItems });
                    }
                    if (scifi && scifi.results) {
                        var sItems = scifi.results.filter(function (x) { return x.poster_path; }).map(function (x) { return formatItem(x, "tv"); });
                        if (sItems.length) feed.categories.push({ title: "Sci-Fi & Fantasy Series", items: sItems });
                    }
                    if (comedy && comedy.results) {
                        var cItems = comedy.results.filter(function (x) { return x.poster_path; }).map(function (x) { return formatItem(x, "tv"); });
                        if (cItems.length) feed.categories.push({ title: "Comedy Series", items: cItems });
                    }
                    if (crime && crime.results) {
                        var crItems = crime.results.filter(function (x) { return x.poster_path; }).map(function (x) { return formatItem(x, "tv"); });
                        if (crItems.length) feed.categories.push({ title: "Crime & Mystery", items: crItems });
                    }
                    cache.feeds[cacheKey] = feed;
                    return feed;
                });
            } else if (tab === "anime") {
                return Promise.all([
                    tmdbGet("/discover/tv?with_genres=16&with_original_language=ja&sort_by=popularity.desc", lang),
                    tmdbGet("/discover/movie?with_genres=16&with_original_language=ja&sort_by=popularity.desc", lang),
                    tmdbGet("/discover/tv?with_genres=16&with_original_language=ja&sort_by=vote_average.desc&vote_count.gte=200", lang),
                    tmdbGet("/discover/tv?with_genres=16,10759&with_original_language=ja&sort_by=popularity.desc", lang),
                    tmdbGet("/discover/tv?with_genres=16,10765&with_original_language=ja&sort_by=popularity.desc", lang)
                ]).then(function (res) {
                    var pop = res[0], mov = res[1], top = res[2], act = res[3], fan = res[4];
                    if (pop && pop.results) {
                        var items = pop.results.filter(function (x) { return x.poster_path; }).map(function (x) { return formatItem(x, "tv"); });
                        if (items.length) {
                            feed.featured = items[0];
                            feed.categories.push({ title: "Top 10 Anime Series", isTop10: true, items: items.slice(0, 10) });
                            feed.categories.push({ title: "Popular Anime Worldwide", items: items });
                        }
                    }
                    if (mov && mov.results) {
                        var mItems = mov.results.filter(function (x) { return x.poster_path; }).map(function (x) { return formatItem(x, "movie"); });
                        if (mItems.length) feed.categories.push({ title: "Acclaimed Anime Features", items: mItems });
                    }
                    if (top && top.results) {
                        var tItems = top.results.filter(function (x) { return x.poster_path; }).map(function (x) { return formatItem(x, "tv"); });
                        if (tItems.length) feed.categories.push({ title: "Masterpiece Anime Series", items: tItems });
                    }
                    if (act && act.results) {
                        var aItems = act.results.filter(function (x) { return x.poster_path; }).map(function (x) { return formatItem(x, "tv"); });
                        if (aItems.length) feed.categories.push({ title: "High-Octane Shonen & Action", items: aItems });
                    }
                    if (fan && fan.results) {
                        var fItems = fan.results.filter(function (x) { return x.poster_path; }).map(function (x) { return formatItem(x, "tv"); });
                        if (fItems.length) feed.categories.push({ title: "Fantasy & Supernatural Anime", items: fItems });
                    }
                    cache.feeds[cacheKey] = feed;
                    return feed;
                });
            } else {
                // Home tab
                return Promise.all([
                    tmdbGet("/trending/all/week", lang),
                    tmdbGet("/movie/popular", lang),
                    tmdbGet("/tv/popular", lang),
                    tmdbGet("/movie/top_rated", lang),
                    tmdbGet("/tv/top_rated", lang),
                    tmdbGet("/discover/movie?with_genres=28&sort_by=popularity.desc", lang),
                    tmdbGet("/discover/tv?with_genres=16&sort_by=popularity.desc", lang)
                ]).then(function (res) {
                    var trend = res[0], popMov = res[1], popTv = res[2], topMov = res[3], topTv = res[4], act = res[5], ani = res[6];
                    if (trend && trend.results) {
                        var items = trend.results.filter(function (x) { return x.poster_path; }).map(function (x) { return formatItem(x); });
                        if (items.length) {
                            feed.featured = items[0];
                            feed.categories.push({ title: "Trending Across PlayStation", isTop10: true, items: items.slice(0, 10) });
                        }
                    }
                    if (popMov && popMov.results) {
                        var mItems = popMov.results.filter(function (x) { return x.poster_path; }).map(function (x) { return formatItem(x, "movie"); });
                        if (mItems.length) feed.categories.push({ title: "Blockbuster Movies", items: mItems });
                    }
                    if (popTv && popTv.results) {
                        var tItems = popTv.results.filter(function (x) { return x.poster_path; }).map(function (x) { return formatItem(x, "tv"); });
                        if (tItems.length) feed.categories.push({ title: "Top Television Series", items: tItems });
                    }
                    if (topMov && topMov.results) {
                        var tmItems = topMov.results.filter(function (x) { return x.poster_path; }).map(function (x) { return formatItem(x, "movie"); });
                        if (tmItems.length) feed.categories.push({ title: "Critically Acclaimed Films", items: tmItems });
                    }
                    if (topTv && topTv.results) {
                        var ttItems = topTv.results.filter(function (x) { return x.poster_path; }).map(function (x) { return formatItem(x, "tv"); });
                        if (ttItems.length) feed.categories.push({ title: "Hall of Fame TV Shows", items: ttItems });
                    }
                    if (act && act.results) {
                        var aItems = act.results.filter(function (x) { return x.poster_path; }).map(function (x) { return formatItem(x, "movie"); });
                        if (aItems.length) feed.categories.push({ title: "Explosive Action Hits", items: aItems });
                    }
                    if (ani && ani.results) {
                        var anItems = ani.results.filter(function (x) { return x.poster_path; }).map(function (x) { return formatItem(x, "tv"); });
                        if (anItems.length) feed.categories.push({ title: "Anime & Animation Favorites", items: anItems });
                    }
                    cache.feeds[cacheKey] = feed;
                    return feed;
                });
            }
        }

        function getDetails(id, type, provider) {
            var lang = getLang(provider);
            var mtype = (type === "tv") ? "tv" : "movie";
            var cacheKey = "det_" + mtype + "_" + id + "_" + lang;
            if (cache.details[cacheKey]) {
                return Promise.resolve(cache.details[cacheKey]);
            }

            return tmdbGet("/" + mtype + "/" + id + "?append_to_response=credits,recommendations", lang).then(function (d) {
                var title = d.title || d.name || d.original_title || "Unknown";
                var date = d.release_date || d.first_air_date || "";
                var year = date ? date.substring(0, 4) : "";
                var rating = Math.round((d.vote_average || 0) * 10) / 10;
                var matchPct = rating > 0 ? Math.min(99, Math.max(75, Math.round(rating * 10 + 15))) : 92;

                var genres = (d.genres || []).map(function (g) { return g.name; }).slice(0, 3).join(", ");
                var castList = (d.credits && d.credits.cast) ? d.credits.cast.slice(0, 4).map(function (c) { return c.name; }).join(", ") : "";
                var director = "";
                if (d.credits && d.credits.crew) {
                    var dir = d.credits.crew.find(function (c) { return c.job === "Director"; });
                    if (dir) director = dir.name;
                }

                var durationStr = "";
                if (mtype === "movie" && d.runtime) {
                    var h = Math.floor(d.runtime / 60);
                    var m = d.runtime % 60;
                    durationStr = (h > 0 ? h + "h " : "") + m + "m";
                } else if (mtype === "tv" && d.number_of_seasons) {
                    durationStr = d.number_of_seasons + " Season" + (d.number_of_seasons > 1 ? "s" : "");
                }

                var recs = (d.recommendations && d.recommendations.results) ? d.recommendations.results.filter(function (x) { return x.poster_path; }).slice(0, 10).map(function (x) { return formatItem(x, mtype); }) : [];

                var seasons = [];
                if (mtype === "tv" && d.seasons) {
                    seasons = d.seasons.filter(function (s) { return s.season_number > 0; }).map(function (s) {
                        return {
                            season_number: s.season_number,
                            name: s.name || ("Season " + s.season_number),
                            episode_count: s.episode_count || 0
                        };
                    });
                }

                var details = {
                    id: String(d.id),
                    title: title,
                    type: mtype,
                    year: year,
                    overview: d.overview || "",
                    rating: String(rating),
                    match: matchPct + "% Match",
                    poster: d.poster_path ? ("https://image.tmdb.org/t/p/w500" + d.poster_path) : "",
                    backdrop: d.backdrop_path ? ("https://image.tmdb.org/t/p/w1280" + d.backdrop_path) : "",
                    certification: "16+",
                    duration: durationStr || "2h 00m",
                    cast: castList || "Not listed",
                    genres: genres || "Drama",
                    director: director || "Not listed",
                    seasons: seasons,
                    recommendations: recs
                };

                cache.details[cacheKey] = details;
                return details;
            });
        }

        function getSeason(tvId, seasonNum, provider) {
            var lang = getLang(provider);
            var cacheKey = "s_" + tvId + "_" + seasonNum + "_" + lang;
            if (cache.seasons[cacheKey]) {
                return Promise.resolve(cache.seasons[cacheKey]);
            }

            return tmdbGet("/tv/" + tvId + "/season/" + seasonNum, lang).then(function (d) {
                var eps = [];
                if (d.episodes) {
                    eps = d.episodes.map(function (ep) {
                        var runtimeStr = ep.runtime ? (ep.runtime + "m") : "";
                        return {
                            episode_number: ep.episode_number,
                            name: ep.name || ("Episode " + ep.episode_number),
                            overview: ep.overview || "",
                            still: ep.still_path ? ("https://image.tmdb.org/t/p/w300" + ep.still_path) : "",
                            runtime: runtimeStr
                        };
                    });
                }
                var res = { episodes: eps };
                cache.seasons[cacheKey] = res;
                return res;
            });
        }

        function search(query, provider) {
            var lang = getLang(provider);
            if (!query || !query.trim()) return Promise.resolve({ results: [] });

            return tmdbGet("/search/multi?query=" + encodeURIComponent(query.trim()), lang).then(function (d) {
                var results = [];
                if (d.results) {
                    results = d.results.filter(function (x) {
                        return (x.media_type === "movie" || x.media_type === "tv") && x.poster_path;
                    }).map(function (x) {
                        return formatItem(x, x.media_type);
                    });
                }
                return { results: results };
            });
        }

        return {
            getFeed: getFeed,
            getDetails: getDetails,
            getSeason: getSeason,
            search: search,
            formatItem: formatItem
        };
    })();

    // ==========================================
    // Tab & Catalog Loading Engine
    // ==========================================

    function loadTabFeed(tabName) {
        state.currentTab = tabName;
        updateTabUI(tabName);

        var billboard = document.getElementById("billboard-section");
        var continueSec = document.getElementById("continue-watching-section");

        if (tabName === "search") {
            state.inSearch = true;
            if (billboard) billboard.style.display = "none";
            if (continueSec) continueSec.style.display = "none";
            window.scrollTo(0, 0);
            openSearchView();
            return;
        }

        state.inSearch = false;
        var searchInput = document.getElementById("search-input");
        if (searchInput && searchInput.value) {
            searchInput.value = "";
            updateSearchClearBtn();
        }

        if (tabName === "settings") {
            openSettingsModal();
            return;
        }

        if (tabName === "list") {
            if (billboard) billboard.style.display = "none";
            if (continueSec) continueSec.style.display = "none";
            renderFullWatchlist();
            return;
        }

        if (billboard) billboard.style.display = "";
        if (continueSec) continueSec.style.display = "";

        var container = document.getElementById("catalog-rows");
        container.innerHTML = '<div style="text-align:center;padding:70px;color:#808080;font-size:18px;font-weight:700;">Loading PStream Experience...</div>';

        // High-Speed Direct TMDb Fetch (Autonomous & Zero-PC Required)
        TMDb.getFeed(tabName, state.activeProvider)
            .then(function (data) {
                state.catalogFeed = data;
                renderFeed(data);
                renderContinueWatching();
            })
            .catch(function (err) {
                log("TMDb direct fetch fallback to local /api/feed: " + err);
                fetch("/api/feed?tab=" + tabName + "&lang=" + encodeURIComponent(state.activeProvider || "eng"))
                    .then(function (res) { return res.json(); })
                    .then(function (data) {
                        state.catalogFeed = data;
                        renderFeed(data);
                        renderContinueWatching();
                    })
                    .catch(function (e) {
                        log("Feed loading error: " + e);
                        container.innerHTML = '<div style="text-align:center;padding:70px;color:#e50914;font-size:18px;">Failed to load catalog feed. Please check your network connection.</div>';
                    });
            });
    }

    function updateTabUI(tabName) {
        var tabs = Array.from(document.querySelectorAll("#nav-tabs .nav-tab"));
        tabs.forEach(function (t) {
            if (t.dataset.tab === tabName) {
                t.classList.add("active");
            } else {
                t.classList.remove("active");
            }
        });
    }

    function renderFeed(data) {
        if (!data) return;

        // Render Spotlight Billboard
        if (data.featured) {
            var f = data.featured;
            var bTitle = document.getElementById("billboard-title");
            var bDesc = document.getElementById("billboard-desc");
            var bBackdrop = document.getElementById("billboard-backdrop");
            var bMatch = document.getElementById("billboard-match");
            var bYear = document.getElementById("billboard-year");

            if (bTitle) bTitle.innerText = f.title;
            if (bDesc) bDesc.innerText = f.overview || "Now streaming in Ultra-HD quality.";
            if (bBackdrop && f.backdrop) bBackdrop.src = f.backdrop;
            if (bMatch) bMatch.innerText = f.match || "98% Match";
            if (bYear) bYear.innerText = f.year || "2024";

            var playBtn = document.getElementById("billboard-play-btn");
            var infoBtn = document.getElementById("billboard-info-btn");

            if (playBtn) {
                playBtn.onclick = function () {
                    playMedia(f);
                };
            }
            if (infoBtn) {
                infoBtn.onclick = function () {
                    openDetailModal(f.id, f.type);
                };
            }
        }

        // Render Rows & Shelves
        var container = document.getElementById("catalog-rows");
        container.innerHTML = "";

        if (!data.categories || data.categories.length === 0) {
            container.innerHTML = '<div style="color:#808080;padding:60px;font-size:18px;">No titles available for this section.</div>';
            return;
        }

        data.categories.forEach(function (cat) {
            var section = document.createElement("section");
            section.className = "shelf-section";

            var header = document.createElement("div");
            header.className = "shelf-header";
            header.innerHTML =
                '<h2 class="shelf-title">' +
                    '<span>' + cat.title + '</span>' +
                    '<svg class="shelf-title-arrow" viewBox="0 0 24 24"><path d="M8.59 16.59L13.17 12 8.59 7.41 10 6l6 6-6 6-1.41-1.41z"/></svg>' +
                '</h2>';
            section.appendChild(header);

            var carousel = document.createElement("div");
            carousel.className = "card-carousel";

            cat.items.forEach(function (item) {
                var card = document.createElement("div");
                card.className = "netflix-card";
                card.dataset.id = item.id;
                card.dataset.type = item.type;

                card.innerHTML =
                    '<img src="' + (item.poster || 'https://via.placeholder.com/500x750') + '" alt="' + item.title + '" loading="lazy">' +
                    '<div class="card-badge-top-left">' + (item.type === "tv" ? "TV" : "FILM") + '</div>' +
                    '<div class="card-badge-top-right">' +
                        '<svg viewBox="0 0 24 24"><path d="M12 17.27L18.18 21l-1.64-7.03L22 9.24l-7.19-.61L12 2 9.19 8.63 2 9.24l5.46 4.73L5.82 21z"/></svg> ' +
                        (item.rating || "8.0") +
                    '</div>' +
                    '<div class="card-info-overlay">' +
                        '<div class="card-title-text">' + item.title + '</div>' +
                    '</div>';

                card.addEventListener("click", function () {
                    openDetailModal(item.id, item.type);
                });

                carousel.appendChild(card);
            });

            section.appendChild(carousel);
            container.appendChild(section);
        });
    }

    function renderFullWatchlist() {
        var container = document.getElementById("catalog-rows");
        container.innerHTML = "";

        var section = document.createElement("section");
        section.className = "shelf-section";
        section.innerHTML = '<h2 class="shelf-title" style="margin-bottom:25px;"><span>Continue Watching & Saved Titles</span></h2>';

        var list = state.continueList;
        if (!list || list.length === 0) {
            section.innerHTML += '<div style="color:#808080;padding:40px;font-size:18px;">You haven\'t started watching any titles yet. Start streaming to add items here!</div>';
            container.appendChild(section);
            return;
        }

        var grid = document.createElement("div");
        grid.className = "recommendations-grid";
        grid.style.gridTemplateColumns = "repeat(auto-fill, minmax(215px, 1fr))";

        list.forEach(function (item) {
            var card = document.createElement("div");
            card.className = "netflix-card";
            card.style.flex = "none";
            card.style.width = "100%";
            card.dataset.id = item.id;
            card.dataset.type = item.type;

            var tag = (item.type === "tv") ? ("S" + item.season + ":E" + item.episode) : "MOVIE";
            var pct = 0;
            if (item.currentTime && item.duration) {
                pct = Math.min(100, Math.round((item.currentTime / item.duration) * 100));
            }
            var timeBadge = (item.currentTime > 0) ? formatTime(item.currentTime) : "";

            card.innerHTML =
                '<img src="' + item.poster + '" alt="' + item.title + '">' +
                '<div class="card-badge-top-left">' + tag + '</div>' +
                (pct > 0 ? '<div class="card-progress-bar-wrap"><div class="card-progress-bar-fill" style="width:' + pct + '%;"></div></div>' : '') +
                (timeBadge ? '<div class="card-progress-timestamp">' + timeBadge + '</div>' : '') +
                '<div class="card-info-overlay">' +
                    '<div class="card-title-text">' + item.title + '</div>' +
                '</div>';

            card.addEventListener("click", function () {
                var saved = findContinueItem(item.id) || item;
                playMedia(saved, saved.season || 1, saved.episode || 1, saved.epTitle || "");
            });

            grid.appendChild(card);
        });

        section.appendChild(grid);
        container.appendChild(section);
    }

    // ==========================================
    // Netflix "More Info" Detail Modal View
    // ==========================================

    function openDetailModal(mediaId, mediaType) {
        state.view = "detail";
        modalNav.section = "play";
        modalNav.seasonIndex = 0;
        modalNav.episodeIndex = 0;
        modalNav.recIndex = 0;

        detailModal.style.display = "flex";

        document.getElementById("modal-title").innerText = "Loading...";
        document.getElementById("modal-overview").innerText = "Fetching details from TMDb...";
        document.getElementById("modal-cast").innerText = "...";
        document.getElementById("modal-genres").innerText = "...";
        document.getElementById("modal-director").innerText = "...";
        document.getElementById("modal-episodes-section").style.display = "none";
        document.getElementById("modal-recs-grid").innerHTML = "";

        var playBtn = document.getElementById("modal-play-btn");
        if (playBtn) setFocus(playBtn);

        var mtype = (mediaType === "tv") ? "tv" : "movie";

        // Standalone TMDb Direct Details
        TMDb.getDetails(mediaId, mtype, state.activeProvider)
            .then(function (data) {
                if (!data.type) data.type = mtype;
                state.currentDetails = data;
                renderDetailModal(data);
            })
            .catch(function (err) {
                log("TMDb direct details error, trying local fallback: " + err);
                var epUrl = "/api/details?id=" + encodeURIComponent(mediaId) + "&type=" + mtype + "&lang=en-US";
                fetch(epUrl)
                    .then(function (r) { return r.json(); })
                    .then(function (data) {
                        if (!data.type) data.type = mtype;
                        state.currentDetails = data;
                        renderDetailModal(data);
                    })
                    .catch(function (e) {
                        log("Error loading media details: " + e);
                    });
            });
    }

    function renderDetailModal(data) {
        if (!data) return;

        var bDrop = document.getElementById("modal-backdrop");
        if (bDrop && data.backdrop) bDrop.src = data.backdrop;

        document.getElementById("modal-title").innerText = data.title;
        document.getElementById("modal-overview").innerText = data.overview || "No synopsis available.";
        document.getElementById("modal-match").innerText = data.match || "98% Match";
        document.getElementById("modal-year").innerText = data.year || "2024";
        document.getElementById("modal-cert").innerText = data.certification || "16+";
        document.getElementById("modal-duration").innerText = data.duration || "2h 15m";

        document.getElementById("modal-cast").innerText = data.cast || "Not listed";
        document.getElementById("modal-genres").innerText = data.genres || "Not listed";
        document.getElementById("modal-director").innerText = data.director || "Not listed";

        var playBtn = document.getElementById("modal-play-btn");
        if (playBtn) {
            playBtn.onclick = function () {
                var saved = findContinueItem(data.id);
                var sNum = (saved && saved.season) ? saved.season : 1;
                var eNum = (saved && saved.episode) ? saved.episode : 1;
                var epTitle = (saved && saved.epTitle) ? saved.epTitle : "";
                var itemToPlay = {
                    id: data.id,
                    title: data.title,
                    type: data.type,
                    poster: data.poster,
                    backdrop: data.backdrop,
                    year: data.year,
                    rating: data.rating,
                    match: data.match
                };
                detailModal.style.display = "none";
                playMedia(itemToPlay, sNum, eNum, epTitle);
            };
        }

        // TV Show Season & Episodes List
        var epSec = document.getElementById("modal-episodes-section");
        if (data.type === "tv" && data.seasons && data.seasons.length > 0) {
            epSec.style.display = "block";
            renderModalSeasons(data);
        } else {
            epSec.style.display = "none";
        }

        // Recommendations Grid
        var recsGrid = document.getElementById("modal-recs-grid");
        recsGrid.innerHTML = "";
        if (data.recommendations && data.recommendations.length > 0) {
            data.recommendations.forEach(function (rec) {
                var card = document.createElement("div");
                card.className = "recommendation-card";
                card.innerHTML =
                    '<img src="' + (rec.poster || 'https://via.placeholder.com/300x450') + '" alt="' + rec.title + '">' +
                    '<div class="rec-card-body">' +
                        '<div class="rec-card-title">' + rec.title + '</div>' +
                        '<div style="font-size:12px;color:#aaa;margin-top:4px;">' + (rec.rating || "8.0") + ' Rating</div>' +
                    '</div>';

                card.onclick = function () {
                    openDetailModal(rec.id, rec.type);
                };

                recsGrid.appendChild(card);
            });
        }
    }

    function renderModalSeasons(data) {
        var pillsWrap = document.getElementById("modal-season-pills");
        pillsWrap.innerHTML = "";

        data.seasons.forEach(function (s, idx) {
            var pill = document.createElement("button");
            pill.className = "season-pill" + (idx === 0 ? " active" : "");
            pill.innerText = s.name || ("Season " + s.season_number);
            pill.dataset.season = s.season_number;

            pill.onclick = function () {
                document.querySelectorAll("#modal-season-pills .season-pill").forEach(function (p) { p.classList.remove("active"); });
                pill.classList.add("active");
                modalNav.seasonIndex = idx;
                loadModalEpisodes(data.id, s.season_number);
            };

            pillsWrap.appendChild(pill);
        });

        if (data.seasons.length > 0) {
            loadModalEpisodes(data.id, data.seasons[0].season_number);
        }
    }

    function loadModalEpisodes(tvId, seasonNum) {
        var epList = document.getElementById("modal-episodes-list");
        epList.innerHTML = '<div style="color:#888;padding:20px;">Loading season episodes...</div>';

        function renderEps(eps) {
            epList.innerHTML = "";
            if (eps.length === 0) {
                epList.innerHTML = '<div style="color:#888;padding:20px;">No episodes available.</div>';
                return;
            }

            eps.forEach(function (ep) {
                var item = document.createElement("div");
                item.className = "episode-item";

                var thumb = ep.still || (state.currentDetails ? state.currentDetails.backdrop : "");
                item.innerHTML =
                    '<div class="episode-thumb-wrap">' +
                        '<img src="' + thumb + '" alt="' + ep.name + '">' +
                        '<div class="episode-play-overlay">' +
                            '<svg viewBox="0 0 24 24"><path d="M8 5v14l11-7z"/></svg>' +
                        '</div>' +
                    '</div>' +
                    '<div class="episode-meta-col">' +
                        '<div class="episode-title-row">' +
                            '<span>' + ep.episode_number + '. ' + ep.name + '</span>' +
                            '<span class="episode-runtime">' + (ep.runtime || "") + '</span>' +
                        '</div>' +
                        '<div class="episode-synopsis">' + (ep.overview || "No overview available.") + '</div>' +
                    '</div>';

                item.onclick = function () {
                    var mediaObj = {
                        id: state.currentDetails.id,
                        title: state.currentDetails.title,
                        type: "tv",
                        poster: state.currentDetails.poster,
                        backdrop: state.currentDetails.backdrop,
                        year: state.currentDetails.year,
                        rating: state.currentDetails.rating,
                        match: state.currentDetails.match
                    };
                    detailModal.style.display = "none";
                    playMedia(mediaObj, seasonNum, ep.episode_number, ep.name);
                };

                epList.appendChild(item);
            });
        }

        TMDb.getSeason(tvId, seasonNum, state.activeProvider)
            .then(function (data) {
                renderEps(data.episodes || []);
            })
            .catch(function (err) {
                log("TMDb direct season error, trying local fallback: " + err);
                fetch("/api/season?id=" + tvId + "&season=" + seasonNum + "&lang=" + encodeURIComponent(state.activeProvider || "eng"))
                    .then(function (r) { return r.json(); })
                    .then(function (data) {
                        renderEps(data.episodes || []);
                    })
                    .catch(function (e) {
                        log("Error fetching season episodes: " + e);
                    });
            });
    }

    function closeDetailModal() {
        detailModal.style.display = "none";
        state.view = "browse";
        state.currentDetails = null;
        if (state.inSearch) {
            var searchGrid = document.querySelector("#catalog-rows .recommendations-grid");
            var firstCard = searchGrid ? searchGrid.querySelector(".netflix-card") : null;
            if (firstCard) {
                nav.tier = "shelves";
                nav.shelfIndex = 0;
                nav.cardIndex = 0;
                setFocus(firstCard);
                return;
            }
        }
        focusBillboardPlay();
    }

    // ==========================================
    // DOM Event Listeners & Interaction Wiring
    // ==========================================

    function setupDomEventListeners() {
        // Nav Tabs Click
        document.querySelectorAll("#nav-tabs .nav-tab").forEach(function (tab) {
            tab.onclick = function () {
                loadTabFeed(tab.dataset.tab);
            };
        });

        // Search Input Events
        var searchInput = document.getElementById("search-input");
        if (searchInput) {
            searchInput.oninput = function () {
                var q = searchInput.value.trim();
                updateSearchClearBtn();
                if (q.length > 0) {
                    state.inSearch = true;
                    state.currentTab = "search";
                    updateTabUI("search");
                    var billboard = document.getElementById("billboard-section");
                    if (billboard) billboard.style.display = "none";
                    var continueSec = document.getElementById("continue-watching-section");
                    if (continueSec) continueSec.style.display = "none";
                    window.scrollTo(0, 0);

                    TMDb.search(q, state.activeProvider)
                        .then(function (results) {
                            renderSearchResults(results);
                        })
                        .catch(function () {
                            fetch("/api/search?q=" + encodeURIComponent(q) + "&lang=" + encodeURIComponent(state.activeProvider || "eng"))
                                .then(function (r) { return r.json(); })
                                .then(function (results) {
                                    renderSearchResults(results);
                                });
                        });
                } else if (q.length === 0) {
                    if (state.currentTab === "search") {
                        openSearchView();
                    } else {
                        state.inSearch = false;
                        loadTabFeed(state.currentTab || "home");
                    }
                }
            };
        }

        var searchClearBtn = document.getElementById("search-clear-btn");
        if (searchClearBtn) {
            searchClearBtn.onclick = function (e) {
                e.preventDefault();
                e.stopPropagation();
                clearSearch();
                var sIn = document.getElementById("search-input");
                if (sIn) sIn.focus();
            };
        }

        // Detail Modal Close
        var detailClose = document.getElementById("modal-close-btn");
        if (detailClose) detailClose.onclick = closeDetailModal;

        // Player Controls
        var exitBtn = document.getElementById("player-exit-btn");
        if (exitBtn) exitBtn.onclick = exitPlayer;

        var rewBtn = document.getElementById("player-rewind-10");
        if (rewBtn) rewBtn.onclick = function () { seekRelative(-(state.skipStep || 10)); };

        var playPauseBtn = document.getElementById("player-center-playpause");
        if (playPauseBtn) playPauseBtn.onclick = togglePlayPause;

        var fwdBtn = document.getElementById("player-forward-10");
        if (fwdBtn) fwdBtn.onclick = function () { seekRelative(state.skipStep || 10); };

        var aspectBtn = document.getElementById("player-aspect-btn");
        if (aspectBtn) aspectBtn.onclick = toggleAspectRatio;

        var prevEpBtn = document.getElementById("player-prev-ep-btn");
        if (prevEpBtn) prevEpBtn.onclick = playPreviousEpisode;

        var epDrawerBtn = document.getElementById("player-episodes-btn");
        if (epDrawerBtn) epDrawerBtn.onclick = toggleEpisodesDrawer;

        var audioDrawerBtn = document.getElementById("player-audio-sub-btn");
        if (audioDrawerBtn) audioDrawerBtn.onclick = toggleAudioDrawer;

        var nextEpBtn = document.getElementById("player-next-ep-btn");
        if (nextEpBtn) nextEpBtn.onclick = playNextEpisode;

        var scrubberTrack = document.getElementById("player-scrubber-track");
        if (scrubberTrack) {
            scrubberTrack.onclick = function (e) {
                var rect = scrubberTrack.getBoundingClientRect();
                var clickX = e.clientX - rect.left;
                var pct = Math.max(0, Math.min(1, clickX / rect.width));
                if (playerVideo && state.duration > 0) {
                    playerVideo.currentTime = pct * state.duration;
                }
            };
        }

        // Drawers Close Buttons
        var drawerClose = document.getElementById("drawer-close-btn");
        if (drawerClose) drawerClose.onclick = closeEpisodesDrawer;

        var audioClose = document.getElementById("audio-drawer-close-btn");
        if (audioClose) audioClose.onclick = closeAudioDrawer;

        // Error Dialog Buttons
        var errRetry = document.getElementById("player-error-retry");
        if (errRetry) errRetry.onclick = function () {
            hideErrorModal();
            if (state.currentItem) {
                playMedia(state.currentItem, state.currentSeason, state.currentEpisode, state.currentEpTitle);
            }
        };

        var errSettings = document.getElementById("player-error-settings");
        if (errSettings) errSettings.onclick = function () {
            hideErrorModal();
            openSettingsModal();
        };

        var errExit = document.getElementById("player-error-exit");
        if (errExit) errExit.onclick = exitPlayer;

        // Context Menu Buttons
        document.querySelectorAll(".context-menu-item").forEach(function (btn) {
            btn.onclick = function () {
                executeContextAction(btn.dataset.action);
            };
        });

        // Provider Options in Welcome Modal
        document.querySelectorAll(".provider-option-btn").forEach(function (btn) {
            btn.onclick = function () {
                selectProvider(btn.dataset.provider);
            };
        });

        // Settings Modal Wiring
        var settingsClose = document.getElementById("settings-close-btn");
        if (settingsClose) settingsClose.onclick = closeSettingsModal;

        document.querySelectorAll("#settings-provider-pills .setting-pill").forEach(function (pill) {
            pill.onclick = function () {
                selectProvider(pill.dataset.provider);
            };
        });

        document.querySelectorAll("#settings-skip-pills .setting-pill").forEach(function (pill) {
            pill.onclick = function () {
                setSkipStep(parseInt(pill.dataset.skip, 10));
            };
        });

        document.querySelectorAll("#settings-autoplay-pills .setting-pill").forEach(function (pill) {
            pill.onclick = function () {
                setAutoplay(pill.dataset.autoplay);
            };
        });

        document.querySelectorAll("#settings-subsize-pills .setting-pill").forEach(function (pill) {
            pill.onclick = function () {
                setSubSize(pill.dataset.subsize);
            };
        });

        var clearHistBtn = document.getElementById("btn-clear-history");
        if (clearHistBtn) {
            clearHistBtn.onclick = function () {
                localStorage.removeItem(STORAGE_KEY);
                state.continueList = [];
                renderContinueWatching();
            };
        }

        var clearFavBtn = document.getElementById("btn-clear-favorites");
        if (clearFavBtn) {
            clearFavBtn.onclick = function () {
                localStorage.removeItem(FAVORITES_KEY);
                state.favoritesList = [];
            };
        }

        var resetBtn = document.getElementById("btn-reset-settings");
        if (resetBtn) {
            resetBtn.onclick = resetAllSettings;
        }
    }

    function renderSearchResults(data) {
        var container = document.getElementById("catalog-rows");
        container.innerHTML = "";

        // Hide billboard and continue watching so search results are at the very top!
        var billboard = document.getElementById("billboard-section");
        if (billboard) billboard.style.display = "none";
        var continueSec = document.getElementById("continue-watching-section");
        if (continueSec) continueSec.style.display = "none";
        window.scrollTo(0, 0);

        var results = (data && data.results) ? data.results : (Array.isArray(data) ? data : []);

        var section = document.createElement("section");
        section.className = "shelf-section";
        section.style.paddingTop = "10px";
        section.innerHTML = '<h2 class="shelf-title" style="margin-bottom:20px;"><span>Search Results</span></h2>';

        if (!results || results.length === 0) {
            section.innerHTML += '<div style="color:#808080;padding:30px;font-size:18px;font-weight:600;">No titles found. Try another search term.</div>';
            container.appendChild(section);
            return;
        }

        var grid = document.createElement("div");
        grid.className = "recommendations-grid";
        grid.style.gridTemplateColumns = "repeat(auto-fill, minmax(215px, 1fr))";

        results.forEach(function (item) {
            var card = document.createElement("div");
            card.className = "netflix-card";
            card.style.flex = "none";
            card.style.width = "100%";
            card.dataset.id = item.id;
            card.dataset.type = item.type;

            card.innerHTML =
                '<img src="' + (item.poster || 'https://via.placeholder.com/500x750') + '" alt="' + item.title + '">' +
                '<div class="card-badge-top-left">' + (item.type === "tv" ? "TV" : "FILM") + '</div>' +
                '<div class="card-info-overlay">' +
                    '<div class="card-title-text">' + item.title + '</div>' +
                '</div>';

            card.addEventListener("click", function () {
                openDetailModal(item.id, item.type);
            });

            grid.appendChild(card);
        });

        section.appendChild(grid);
        container.appendChild(section);
    }

    function openSearchView() {
        var container = document.getElementById("catalog-rows");
        var billboard = document.getElementById("billboard-section");
        var continueSec = document.getElementById("continue-watching-section");
        if (billboard) billboard.style.display = "none";
        if (continueSec) continueSec.style.display = "none";
        window.scrollTo(0, 0);

        var searchInput = document.getElementById("search-input");
        var q = searchInput ? searchInput.value.trim() : "";

        if (q.length > 0) {
            container.innerHTML = '<div style="text-align:center;padding:50px;color:#808080;font-size:18px;font-weight:700;">Searching PStream...</div>';
            TMDb.search(q, state.activeProvider)
                .then(function (results) {
                    renderSearchResults(results);
                })
                .catch(function () {
                    fetch("/api/search?q=" + encodeURIComponent(q) + "&lang=" + encodeURIComponent(state.activeProvider || "eng"))
                        .then(function (r) { return r.json(); })
                        .then(function (results) {
                            renderSearchResults(results);
                        });
                });
        } else {
            container.innerHTML = "";
            var section = document.createElement("section");
            section.className = "shelf-section";
            section.style.paddingTop = "10px";
            section.innerHTML = 
                '<h2 class="shelf-title" style="margin-bottom:12px;"><span>Search Movies, Series & Anime</span></h2>' +
                '<p style="color:#8e8e8e;font-size:16px;margin-bottom:24px;font-weight:600;">Type in the search bar above using the on-screen keyboard, or browse popular titles below.</p>';

            var grid = document.createElement("div");
            grid.className = "recommendations-grid";
            grid.style.gridTemplateColumns = "repeat(auto-fill, minmax(215px, 1fr))";

            function renderDefaultSearchItems(items) {
                grid.innerHTML = "";
                items.forEach(function (item) {
                    var card = document.createElement("div");
                    card.className = "netflix-card";
                    card.style.flex = "none";
                    card.style.width = "100%";
                    card.dataset.id = item.id;
                    card.dataset.type = item.type;
                    card.innerHTML =
                        '<img src="' + (item.poster || 'https://via.placeholder.com/500x750') + '" alt="' + item.title + '">' +
                        '<div class="card-badge-top-left">' + (item.type === "tv" ? "TV" : "FILM") + '</div>' +
                        '<div class="card-info-overlay"><div class="card-title-text">' + item.title + '</div></div>';
                    card.addEventListener("click", function () {
                        openDetailModal(item.id, item.type);
                    });
                    grid.appendChild(card);
                });
            }

            TMDb.getFeed("home", state.activeProvider)
                .then(function (data) {
                    var items = (data && data.categories && data.categories[0] && data.categories[0].items) ? data.categories[0].items : [];
                    renderDefaultSearchItems(items);
                })
                .catch(function () {
                    fetch("/api/feed?tab=home&lang=" + encodeURIComponent(state.activeProvider || "eng"))
                        .then(function (r) { return r.json(); })
                        .then(function (data) {
                            var items = (data && data.categories && data.categories[0] && data.categories[0].items) ? data.categories[0].items : [];
                            renderDefaultSearchItems(items);
                        });
                });

            section.appendChild(grid);
            container.appendChild(section);
        }

        if (searchInput) {
            setFocus(searchInput);
            searchInput.focus();
            if (searchInput.value.length > 0) {
                try { searchInput.select(); } catch (e) { }
            }
            nav.tier = "search";
        }
    }

    function clearSearch() {
        var searchInput = document.getElementById("search-input");
        if (searchInput) {
            searchInput.value = "";
        }
        updateSearchClearBtn();
        if (state.currentTab === "search") {
            openSearchView();
        } else if (state.inSearch) {
            state.inSearch = false;
            loadTabFeed(state.currentTab || "home");
        }
    }

    function updateSearchClearBtn() {
        var clearBtn = document.getElementById("search-clear-btn");
        var searchInput = document.getElementById("search-input");
        if (clearBtn && searchInput) {
            clearBtn.style.display = searchInput.value.length > 0 ? "block" : "none";
        }
    }

    window.onSearchCommit = function () {
        setTimeout(function () {
            var searchGrid = document.querySelector("#catalog-rows .recommendations-grid");
            var firstCard = searchGrid ? searchGrid.querySelector(".netflix-card") : null;
            if (firstCard) {
                nav.tier = "shelves";
                nav.shelfIndex = 0;
                nav.cardIndex = 0;
                setFocus(firstCard);
            }
        }, 150);
    };

    // ==========================================
    // PlayStation Focus Engine (Silky-Smooth Instant TV Navigation)
    // ==========================================

    // Silky Smooth 60 FPS Viewport Scrolling for PlayStation
    var smoothScrollTimer = null;
    function smoothScrollToY(targetY, duration) {
        if (!duration) duration = 240;
        var startY = window.pageYOffset || document.documentElement.scrollTop;
        var diff = targetY - startY;
        if (Math.abs(diff) < 2) return;
        var startTime = performance.now();

        if (smoothScrollTimer) cancelAnimationFrame(smoothScrollTimer);

        function step(now) {
            var elapsed = now - startTime;
            var progress = Math.min(1, elapsed / duration);
            var ease = 1 - Math.pow(1 - progress, 3);
            window.scrollTo(0, Math.round(startY + diff * ease));
            if (progress < 1) {
                smoothScrollTimer = requestAnimationFrame(step);
            } else {
                smoothScrollTimer = null;
            }
        }
        smoothScrollTimer = requestAnimationFrame(step);
    }

    function setFocus(el) {
        if (!el) return;
        document.querySelectorAll(".tv-focused").forEach(function (node) {
            node.classList.remove("tv-focused");
        });
        el.classList.add("tv-focused");
        nav.focusedElement = el;

        if (el.classList.contains("netflix-card")) {
            var carousel = el.closest(".card-carousel");
            if (carousel) {
                var elLeft = el.offsetLeft;
                var elWidth = el.offsetWidth;
                var carScroll = carousel.scrollLeft;
                var carWidth = carousel.clientWidth;

                if (elLeft < carScroll + 40) {
                    carousel.scrollLeft = Math.max(0, elLeft - 40);
                } else if (elLeft + elWidth > carScroll + carWidth - 40) {
                    carousel.scrollLeft = elLeft + elWidth - carWidth + 40;
                }
            }

            var shelf = el.closest(".shelf-section");
            if (shelf) {
                var shelfRect = shelf.getBoundingClientRect();
                if (shelfRect.top < 90 || shelfRect.bottom > window.innerHeight - 60) {
                    var currentY = window.pageYOffset || document.documentElement.scrollTop;
                    var targetY = Math.max(0, currentY + shelfRect.top - Math.round((window.innerHeight - shelfRect.height) / 2));
                    smoothScrollToY(targetY, 260);
                }
            }
        } else if (el.closest && el.closest("#nav-tabs, #search-box-wrap, #billboard-section")) {
            if (window.pageYOffset > 50) {
                smoothScrollToY(0, 240);
            }
        } else {
            if (el.scrollIntoViewIfNeeded) {
                el.scrollIntoViewIfNeeded({ behavior: "auto", block: "nearest", inline: "nearest" });
            } else {
                el.scrollIntoView({ behavior: "auto", block: "nearest", inline: "nearest" });
            }
        }
    }

    function focusBillboardPlay() {
        nav.tier = "billboard";
        nav.billboardBtnIndex = 0;
        smoothScrollToY(0, 260);
        var playBtn = document.getElementById("billboard-play-btn");
        if (playBtn) setFocus(playBtn);
    }

    // ==========================================
    // Controller Navigation Routing
    // ==========================================

    function handleKey(code, KEY) {
        // Any key inside player wakes the overlay
        if (state.view === "player") {
            var overlay = document.getElementById("player-overlay");
            if (overlay && overlay.classList.contains("osd-hidden")) {
                if (code === KEY.CROSS) {
                    togglePlayPause();
                    wakePlayerOsd();
                    return;
                } else {
                    wakePlayerOsd();
                    return;
                }
            } else {
                wakePlayerOsd();
            }
        }

        // 1. DualSense Options Button -> Open Context Menu for focused title card
        if (code === KEY.OPTIONS) {
            if (state.view === "browse") {
                var targetCard = null;
                if (nav.focusedElement) {
                    if (nav.focusedElement.classList.contains("netflix-card")) {
                        targetCard = nav.focusedElement;
                    } else if (nav.focusedElement.closest) {
                        targetCard = nav.focusedElement.closest(".netflix-card");
                    }
                }
                if (!targetCard) {
                    targetCard = document.querySelector(".netflix-card.tv-focused") ||
                                 document.querySelector("#row-continue .netflix-card") ||
                                 document.querySelector(".shelf-section .netflix-card");
                }
                if (targetCard) {
                    openCardContextMenu(targetCard);
                    return;
                }
                return;
            } else if (state.view === "context") {
                closeCardContextMenu();
                return;
            } else if (state.view === "settings_modal") {
                closeSettingsModal();
                return;
            }
            return;
        }

        // Square button in context view closes menu; in search clears search
        if (code === KEY.SQUARE) {
            if (state.view === "context") {
                closeCardContextMenu();
                return;
            } else if (nav.tier === "search" || (document.activeElement && document.activeElement.id === "search-input")) {
                clearSearch();
                return;
            }
        }

        // 2. L1 / R1 Shoulder Buttons -> Tab Cycling
        if (code === KEY.L1) {
            if (state.view === "browse") {
                cycleTabs(-1);
            } else if (state.view === "settings_modal") {
                closeSettingsModal();
                cycleTabs(-1);
            } else if (state.view === "detail") {
                closeDetailModal();
                cycleTabs(-1);
            } else if (state.view === "player") {
                seekRelative(-(state.skipStep || 10));
            }
            return;
        }

        if (code === KEY.R1) {
            if (state.view === "browse") {
                cycleTabs(1);
            } else if (state.view === "settings_modal") {
                closeSettingsModal();
                cycleTabs(1);
            } else if (state.view === "detail") {
                closeDetailModal();
                cycleTabs(1);
            } else if (state.view === "player") {
                seekRelative(state.skipStep || 10);
            }
            return;
        }

        // 3. Circle (O) -> Back / Close
        if (code === KEY.CIRCLE) {
            if (state.view === "context") {
                closeCardContextMenu();
            } else if (state.view === "provider_modal") {
                closeProviderWelcomeModal();
            } else if (state.view === "settings_modal") {
                closeSettingsModal();
            } else if (state.view === "drawer_episodes") {
                closeEpisodesDrawer();
            } else if (state.view === "drawer_audio") {
                closeAudioDrawer();
            } else if (state.view === "detail") {
                closeDetailModal();
            } else if (state.view === "player") {
                exitPlayer();
            } else if (state.view === "browse" && (state.currentTab === "search" || state.inSearch || (document.getElementById("search-input") && document.getElementById("search-input").value))) {
                clearSearch();
                loadTabFeed("home");
                focusBillboardPlay();
            }
            return;
        }

        // 4. Triangle -> Quick Search Tab
        if (code === KEY.TRIANGLE) {
            if (state.view === "browse") {
                loadTabFeed("search");
            }
            return;
        }

        // 5. Cross (X) -> Select / Activate
        if (code === KEY.CROSS) {
            if (state.view === "player") {
                var overlayEl = document.getElementById("player-overlay");
                if (overlayEl && overlayEl.classList.contains("osd-hidden")) {
                    togglePlayPause();
                    return;
                }
                if (nav.focusedElement && nav.focusedElement.closest("#player-overlay")) {
                    nav.focusedElement.click();
                } else {
                    togglePlayPause();
                }
                return;
            }

            if (nav.focusedElement) {
                nav.focusedElement.click();
            }
            return;
        }

        // 6. Directional Navigation
        if (state.view === "browse") {
            if (code === KEY.UP) navigateBrowse("UP");
            else if (code === KEY.DOWN) navigateBrowse("DOWN");
            else if (code === KEY.LEFT) navigateBrowse("LEFT");
            else if (code === KEY.RIGHT) navigateBrowse("RIGHT");
        } else if (state.view === "player") {
            navigatePlayer(code, KEY);
        } else if (state.view === "detail") {
            navigateModal(code, KEY);
        } else if (state.view === "context") {
            navigateContext(code, KEY);
        } else if (state.view === "drawer_episodes") {
            navigateDrawerEpisodes(code, KEY);
        } else if (state.view === "drawer_audio") {
            navigateDrawerAudio(code, KEY);
        } else if (state.view === "provider_modal" || state.view === "settings_modal") {
            navigateModalGeneric(code, KEY);
        }
    }

    function cycleTabs(dir) {
        var contentTabs = ["search", "home", "movies", "tv", "anime", "list"];
        var idx = contentTabs.indexOf(state.currentTab);
        if (idx === -1) idx = 1;
        var nextIdx = (idx + dir + contentTabs.length) % contentTabs.length;
        loadTabFeed(contentTabs[nextIdx]);
    }

    function navigateBrowse(dir) {
        var tabs = Array.from(document.querySelectorAll("#nav-tabs .nav-tab"));
        var shelves = Array.from(document.querySelectorAll("#main-content .shelf-section")).filter(function (s) {
            return s.style.display !== "none";
        });

        if (dir === "UP") {
            if (nav.tier === "shelves") {
                if (nav.shelfIndex > 0) {
                    nav.shelfIndex--;
                    focusCardInShelf(shelves[nav.shelfIndex], nav.cardIndex);
                } else {
                    var contSec = document.getElementById("continue-watching-section");
                    if (contSec && contSec.style.display !== "none") {
                        nav.tier = "continue";
                        focusCardInShelf(contSec, 0);
                    } else {
                        focusBillboardPlay();
                    }
                }
            } else if (nav.tier === "continue") {
                focusBillboardPlay();
            } else if (nav.tier === "billboard") {
                nav.tier = "tabs";
                if (tabs[nav.tabIndex]) setFocus(tabs[nav.tabIndex]);
            } else if (nav.tier === "tabs") {
                // Navigate UP from tabs into search input
                nav.tier = "search";
                var sInput = document.getElementById("search-input");
                if (sInput) {
                    setFocus(sInput);
                    sInput.focus();
                    if (sInput.value.length > 0) {
                        try { sInput.select(); } catch (e) { }
                    }
                }
            }
        } else if (dir === "DOWN") {
            if (nav.tier === "search") {
                var searchGrid = document.querySelector("#catalog-rows .recommendations-grid");
                var searchCards = searchGrid ? Array.from(searchGrid.querySelectorAll(".netflix-card")) : [];
                var activeIn = document.getElementById("search-input");
                if (activeIn) activeIn.blur();
                if (searchCards.length > 0) {
                    nav.tier = "shelves";
                    nav.shelfIndex = 0;
                    nav.cardIndex = 0;
                    setFocus(searchCards[0]);
                } else {
                    nav.tier = "tabs";
                    if (tabs[nav.tabIndex]) setFocus(tabs[nav.tabIndex]);
                }
            } else if (nav.tier === "tabs") {
                focusBillboardPlay();
            } else if (nav.tier === "billboard") {
                var contSecDown = document.getElementById("continue-watching-section");
                if (contSecDown && contSecDown.style.display !== "none") {
                    nav.tier = "continue";
                    focusCardInShelf(contSecDown, 0);
                } else if (shelves.length > 0) {
                    nav.tier = "shelves";
                    nav.shelfIndex = 0;
                    focusCardInShelf(shelves[0], 0);
                }
            } else if (nav.tier === "continue") {
                if (shelves.length > 0) {
                    nav.tier = "shelves";
                    nav.shelfIndex = 0;
                    focusCardInShelf(shelves[0], 0);
                }
            } else if (nav.tier === "shelves") {
                if (nav.shelfIndex < shelves.length - 1) {
                    nav.shelfIndex++;
                    focusCardInShelf(shelves[nav.shelfIndex], nav.cardIndex);
                }
            }
        } else if (dir === "LEFT") {
            if (nav.tier === "search") {
                var inEl = document.getElementById("search-input");
                if (inEl) inEl.blur();
                nav.tier = "tabs";
                nav.tabIndex = tabs.length - 1;
                if (tabs[nav.tabIndex]) setFocus(tabs[nav.tabIndex]);
            } else if (nav.tier === "tabs") {
                if (nav.tabIndex > 0) {
                    nav.tabIndex--;
                    setFocus(tabs[nav.tabIndex]);
                }
            } else if (nav.tier === "billboard") {
                if (nav.billboardBtnIndex > 0) {
                    nav.billboardBtnIndex--;
                    setFocus(document.getElementById("billboard-play-btn"));
                }
            } else if (nav.tier === "shelves" || nav.tier === "continue") {
                var currentShelfL = (nav.tier === "continue") ? document.getElementById("continue-watching-section") : shelves[nav.shelfIndex];
                if (currentShelfL) {
                    var cardsL = Array.from(currentShelfL.querySelectorAll(".netflix-card"));
                    if (nav.cardIndex > 0) {
                        nav.cardIndex--;
                        setFocus(cardsL[nav.cardIndex]);
                    }
                }
            }
        } else if (dir === "RIGHT") {
            if (nav.tier === "tabs") {
                if (nav.tabIndex < tabs.length - 1) {
                    nav.tabIndex++;
                    setFocus(tabs[nav.tabIndex]);
                } else {
                    // Navigate RIGHT past tabs into search input
                    nav.tier = "search";
                    var sIn = document.getElementById("search-input");
                    if (sIn) {
                        setFocus(sIn);
                        sIn.focus();
                        if (sIn.value.length > 0) {
                            try { sIn.select(); } catch (e) { }
                        }
                    }
                }
            } else if (nav.tier === "billboard") {
                if (nav.billboardBtnIndex < 1) {
                    nav.billboardBtnIndex++;
                    setFocus(document.getElementById("billboard-info-btn"));
                }
            } else if (nav.tier === "shelves" || nav.tier === "continue") {
                var currentShelfR = (nav.tier === "continue") ? document.getElementById("continue-watching-section") : shelves[nav.shelfIndex];
                if (currentShelfR) {
                    var cardsR = Array.from(currentShelfR.querySelectorAll(".netflix-card"));
                    if (nav.cardIndex < cardsR.length - 1) {
                        nav.cardIndex++;
                        setFocus(cardsR[nav.cardIndex]);
                    }
                }
            }
        }
    }

    function focusCardInShelf(shelf, index) {
        if (!shelf) return;
        var cards = Array.from(shelf.querySelectorAll(".netflix-card"));
        if (cards.length > 0) {
            nav.cardIndex = Math.min(index, cards.length - 1);
            setFocus(cards[nav.cardIndex]);
        }
    }

    // Player navigation
    function navigatePlayer(code, KEY) {
        var centerBtns = [
            document.getElementById("player-rewind-10"),
            document.getElementById("player-center-playpause"),
            document.getElementById("player-forward-10")
        ];

        var bottomBtns = [
            document.getElementById("player-aspect-btn"),
            document.getElementById("player-prev-ep-btn"),
            document.getElementById("player-episodes-btn"),
            document.getElementById("player-audio-sub-btn"),
            document.getElementById("player-next-ep-btn")
        ].filter(function (b) { return b && b.style.display !== "none"; });

        var exitBtn = document.getElementById("player-exit-btn");
        var scrubber = document.getElementById("player-scrubber-track");

        if (code === KEY.UP) {
            if (playerNav.tier === "bottom") {
                playerNav.tier = "scrubber";
                setFocus(scrubber);
            } else if (playerNav.tier === "scrubber") {
                playerNav.tier = "center";
                setFocus(centerBtns[playerNav.centerIndex]);
            } else if (playerNav.tier === "center") {
                playerNav.tier = "top";
                setFocus(exitBtn);
            }
        } else if (code === KEY.DOWN) {
            if (playerNav.tier === "top") {
                playerNav.tier = "center";
                setFocus(centerBtns[playerNav.centerIndex]);
            } else if (playerNav.tier === "center") {
                playerNav.tier = "scrubber";
                setFocus(scrubber);
            } else if (playerNav.tier === "scrubber") {
                playerNav.tier = "bottom";
                playerNav.bottomIndex = Math.min(playerNav.bottomIndex, bottomBtns.length - 1);
                setFocus(bottomBtns[playerNav.bottomIndex]);
            }
        } else if (code === KEY.LEFT) {
            if (playerNav.tier === "center") {
                if (playerNav.centerIndex > 0) {
                    playerNav.centerIndex--;
                    setFocus(centerBtns[playerNav.centerIndex]);
                }
            } else if (playerNav.tier === "bottom") {
                if (playerNav.bottomIndex > 0) {
                    playerNav.bottomIndex--;
                    setFocus(bottomBtns[playerNav.bottomIndex]);
                }
            } else if (playerNav.tier === "scrubber") {
                seekRelative(-(state.skipStep || 10));
            }
        } else if (code === KEY.RIGHT) {
            if (playerNav.tier === "center") {
                if (playerNav.centerIndex < centerBtns.length - 1) {
                    playerNav.centerIndex++;
                    setFocus(centerBtns[playerNav.centerIndex]);
                }
            } else if (playerNav.tier === "bottom") {
                if (playerNav.bottomIndex < bottomBtns.length - 1) {
                    playerNav.bottomIndex++;
                    setFocus(bottomBtns[playerNav.bottomIndex]);
                }
            } else if (playerNav.tier === "scrubber") {
                seekRelative(state.skipStep || 10);
            }
        }
    }

    // Modal navigation
    function navigateModal(code, KEY) {
        var playBtn = document.getElementById("modal-play-btn");
        var pills = Array.from(document.querySelectorAll("#modal-season-pills .season-pill"));
        var eps = Array.from(document.querySelectorAll("#modal-episodes-list .episode-item"));
        var recs = Array.from(document.querySelectorAll("#modal-recs-grid .recommendation-card"));

        if (code === KEY.DOWN) {
            if (modalNav.section === "play") {
                if (pills.length > 0) {
                    modalNav.section = "seasons";
                    setFocus(pills[modalNav.seasonIndex]);
                } else if (recs.length > 0) {
                    modalNav.section = "recs";
                    setFocus(recs[modalNav.recIndex]);
                }
            } else if (modalNav.section === "seasons") {
                if (eps.length > 0) {
                    modalNav.section = "episodes";
                    setFocus(eps[modalNav.episodeIndex]);
                }
            } else if (modalNav.section === "episodes") {
                if (modalNav.episodeIndex < eps.length - 1) {
                    modalNav.episodeIndex++;
                    setFocus(eps[modalNav.episodeIndex]);
                } else if (recs.length > 0) {
                    modalNav.section = "recs";
                    setFocus(recs[modalNav.recIndex]);
                }
            }
        } else if (code === KEY.UP) {
            if (modalNav.section === "recs") {
                if (eps.length > 0) {
                    modalNav.section = "episodes";
                    modalNav.episodeIndex = eps.length - 1;
                    setFocus(eps[modalNav.episodeIndex]);
                } else {
                    modalNav.section = "play";
                    setFocus(playBtn);
                }
            } else if (modalNav.section === "episodes") {
                if (modalNav.episodeIndex > 0) {
                    modalNav.episodeIndex--;
                    setFocus(eps[modalNav.episodeIndex]);
                } else if (pills.length > 0) {
                    modalNav.section = "seasons";
                    setFocus(pills[modalNav.seasonIndex]);
                }
            } else if (modalNav.section === "seasons") {
                modalNav.section = "play";
                setFocus(playBtn);
            }
        } else if (code === KEY.LEFT) {
            if (modalNav.section === "seasons" && modalNav.seasonIndex > 0) {
                modalNav.seasonIndex--;
                setFocus(pills[modalNav.seasonIndex]);
                pills[modalNav.seasonIndex].click();
            } else if (modalNav.section === "recs" && modalNav.recIndex > 0) {
                modalNav.recIndex--;
                setFocus(recs[modalNav.recIndex]);
            }
        } else if (code === KEY.RIGHT) {
            if (modalNav.section === "seasons" && modalNav.seasonIndex < pills.length - 1) {
                modalNav.seasonIndex++;
                setFocus(pills[modalNav.seasonIndex]);
                pills[modalNav.seasonIndex].click();
            } else if (modalNav.section === "recs" && modalNav.recIndex < recs.length - 1) {
                modalNav.recIndex++;
                setFocus(recs[modalNav.recIndex]);
            }
        }
    }

    // Context Menu Navigation
    function navigateContext(code, KEY) {
        var items = Array.from(document.querySelectorAll("#context-menu-items .context-menu-item:not([style*='display: none'])"));
        if (items.length === 0) return;
        var curr = items.indexOf(nav.focusedElement);
        if (curr === -1) curr = 0;

        if (code === KEY.DOWN) {
            var next = (curr + 1) % items.length;
            setFocus(items[next]);
        } else if (code === KEY.UP) {
            var prev = (curr - 1 + items.length) % items.length;
            setFocus(items[prev]);
        }
    }

    // Drawer episodes navigation
    function navigateDrawerEpisodes(code, KEY) {
        var eps = Array.from(document.querySelectorAll("#drawer-episodes-list .episode-item"));
        var pills = Array.from(document.querySelectorAll("#drawer-season-pills .season-pill"));
        if (eps.length === 0 && pills.length === 0) return;

        var currEp = eps.indexOf(nav.focusedElement);
        var currPill = pills.indexOf(nav.focusedElement);

        if (code === KEY.DOWN) {
            if (currPill !== -1 && eps.length > 0) {
                setFocus(eps[0]);
            } else if (currEp !== -1 && currEp < eps.length - 1) {
                setFocus(eps[currEp + 1]);
            }
        } else if (code === KEY.UP) {
            if (currEp > 0) {
                setFocus(eps[currEp - 1]);
            } else if (currEp === 0 && pills.length > 0) {
                setFocus(pills[0]);
            }
        } else if (code === KEY.LEFT && currPill > 0) {
            setFocus(pills[currPill - 1]);
        } else if (code === KEY.RIGHT && currPill !== -1 && currPill < pills.length - 1) {
            setFocus(pills[currPill + 1]);
        }
    }

    function navigateDrawerAudio(code, KEY) {
        var items = Array.from(document.querySelectorAll("#player-audio-drawer .track-item"));
        if (items.length === 0) return;
        var curr = items.indexOf(nav.focusedElement);
        if (curr === -1) curr = 0;

        if (code === KEY.DOWN) {
            setFocus(items[(curr + 1) % items.length]);
        } else if (code === KEY.UP) {
            setFocus(items[(curr - 1 + items.length) % items.length]);
        }
    }

    function navigateModalGeneric(code, KEY) {
        var container = (state.view === "provider_modal") ? document.getElementById("provider-welcome-modal") : document.getElementById("settings-modal");
        var btns = Array.from(container.querySelectorAll("button:not([style*='display: none'])"));
        if (btns.length === 0) return;
        var curr = btns.indexOf(nav.focusedElement);
        if (curr === -1) curr = 0;

        if (code === KEY.DOWN || code === KEY.RIGHT) {
            setFocus(btns[(curr + 1) % btns.length]);
        } else if (code === KEY.UP || code === KEY.LEFT) {
            setFocus(btns[(curr - 1 + btns.length) % btns.length]);
        }
    }

    return {
        init: init
    };
})();

window.addEventListener("DOMContentLoaded", function () {
    App.init();
});
