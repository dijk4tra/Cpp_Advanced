const state = {
    busy: false,
    composing: false,
    suggestTimer: null,
    suggestController: null,
    suggestRequestId: 0,
    suggestions: [],
    activeSuggestion: -1
};

const SUGGEST_DEBOUNCE_MS = 200;

const form = document.querySelector("#searchForm");
const queryInput = document.querySelector("#queryInput");
const resultCount = document.querySelector("#resultCount");
const elapsedTime = document.querySelector("#elapsedTime");
const results = document.querySelector("#results");
const clearButton = document.querySelector("#clearButton");
const serverState = document.querySelector("#serverState");
const suggestions = document.querySelector("#suggestions");

function setServerState(kind, text) {
    serverState.classList.remove("ok", "error");
    if (kind) {
        serverState.classList.add(kind);
    }
    serverState.querySelector(".status-value strong").textContent = text;
}

function setEmpty(text = "暂无结果") {
    results.className = "results empty-state";
    results.innerHTML = `
        <div class="empty-mark">⌁</div>
        <p>${escapeHtml(text)}</p>
    `;
    resultCount.textContent = "0";
}

function escapeHtml(value) {
    return String(value)
        .replaceAll("&", "&amp;")
        .replaceAll("<", "&lt;")
        .replaceAll(">", "&gt;")
        .replaceAll('"', "&quot;")
        .replaceAll("'", "&#039;");
}

function safeAbstract(value) {
    return String(value)
        .replaceAll("&", "&amp;")
        .replaceAll("<em>", "\u0000")
        .replaceAll("</em>", "\u0001")
        .replaceAll("<", "&lt;")
        .replaceAll(">", "&gt;")
        .replaceAll("\u0000", "<em>")
        .replaceAll("\u0001", "</em>");
}

async function postJson(endpoint, payload, signal) {
    const response = await fetch(endpoint, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload),
        signal
    });

    const data = await response.json();
    if (!response.ok || data.error) {
        throw new Error(data.message || data.error || "request failed");
    }
    return data;
}

function hideSuggestions() {
    suggestions.classList.add("hidden");
    suggestions.innerHTML = "";
    state.suggestions = [];
    state.activeSuggestion = -1;
    queryInput.setAttribute("aria-expanded", "false");
}

function cancelPendingSuggestions() {
    window.clearTimeout(state.suggestTimer);
    state.suggestTimer = null;
    ++state.suggestRequestId;
    if (state.suggestController) {
        state.suggestController.abort();
        state.suggestController = null;
    }
}

function setActiveSuggestion(index) {
    const items = Array.from(suggestions.querySelectorAll(".suggestion-item"));
    if (!items.length) {
        state.activeSuggestion = -1;
        return;
    }

    state.activeSuggestion = (index + items.length) % items.length;
    items.forEach((item, itemIndex) => {
        const active = itemIndex === state.activeSuggestion;
        item.classList.toggle("active", active);
        item.setAttribute("aria-selected", active ? "true" : "false");
    });
}

function renderSuggestions(items) {
    const normalizedItems = items
        .map((item) => ({
            word: String(item.word || ""),
            distance: item.distance,
            frequency: item.frequency
        }))
        .filter((item) => item.word);

    if (!normalizedItems.length) {
        hideSuggestions();
        return;
    }

    state.suggestions = normalizedItems;
    state.activeSuggestion = -1;
    suggestions.className = "suggestions";
    suggestions.innerHTML = normalizedItems.map((item, index) => `
        <button class="suggestion-item"
                type="button"
                role="option"
                aria-selected="false"
                data-index="${index}">
            <span class="suggestion-word">${escapeHtml(item.word)}</span>
            <span class="suggestion-meta">编辑距离${escapeHtml(item.distance)}&nbsp;&nbsp;词频${escapeHtml(item.frequency)}</span>
        </button>
    `).join("");
    queryInput.setAttribute("aria-expanded", "true");
}

async function loadSuggestions(query) {
    if (state.suggestController) {
        state.suggestController.abort();
    }

    const requestId = ++state.suggestRequestId;
    const controller = new AbortController();
    state.suggestController = controller;

    try {
        const data = await postJson(
            "/api/suggest",
            { query },
            controller.signal
        );
        if (requestId !== state.suggestRequestId) {
            return;
        }
        renderSuggestions(Array.isArray(data.results) ? data.results : []);
    } catch (error) {
        if (error.name !== "AbortError") {
            hideSuggestions();
        }
    } finally {
        if (state.suggestController === controller) {
            state.suggestController = null;
        }
    }
}

function queueSuggestions() {
    if (state.composing) {
        return;
    }

    window.clearTimeout(state.suggestTimer);
    const query = queryInput.value.trim();
    if (!query) {
        cancelPendingSuggestions();
        hideSuggestions();
        return;
    }

    state.suggestTimer = window.setTimeout(() => {
        loadSuggestions(query);
    }, SUGGEST_DEBOUNCE_MS);
}

function applySuggestion(index, searchImmediately) {
    const item = state.suggestions[index];
    if (!item) {
        return;
    }

    queryInput.value = item.word;
    hideSuggestions();
    queryInput.focus();

    if (searchImmediately) {
        performSearch(item.word);
    }
}

function renderWebResults(items) {
    results.className = "results";
    if (!items.length) {
        setEmpty("没有搜索结果");
        return;
    }

    results.innerHTML = items.map((item) => {
        const title = item.title || `文档 ${item.id}`;
        const link = item.link || "";
        const score = Number(item.score || 0).toFixed(6);
        return `
            <article class="result-card">
                <h3 class="result-title">
                    ${link
                        ? `<a href="${escapeHtml(link)}" target="_blank" rel="noreferrer">${escapeHtml(title)}</a>`
                        : escapeHtml(title)}
                </h3>
                ${link ? `<a class="result-link" href="${escapeHtml(link)}" target="_blank" rel="noreferrer">${escapeHtml(link)}</a>` : ""}
                <p class="abstract">${safeAbstract(item.abstract || "")}</p>
                <span class="score">score ${score}</span>
            </article>
        `;
    }).join("");
    resultCount.textContent = String(items.length);
}

async function performSearch(rawQuery) {
    if (state.busy) {
        return;
    }

    const query = rawQuery.trim();
    if (!query) {
        setEmpty("请输入查询内容");
        return;
    }

    cancelPendingSuggestions();
    hideSuggestions();
    state.busy = true;
    const start = performance.now();
    setServerState(null, "Searching");
    results.className = "results empty-state";
    results.innerHTML = `
        <div class="empty-mark">◌</div>
        <p>正在查询</p>
    `;

    try {
        const data = await postJson("/api/search", { query });
        const elapsed = Math.round(performance.now() - start);
        elapsedTime.textContent = `${elapsed} ms`;
        setServerState("ok", "Online");
        renderWebResults(Array.isArray(data.results) ? data.results : []);
    } catch (error) {
        elapsedTime.textContent = "-";
        resultCount.textContent = "0";
        setServerState("error", "Offline");
        results.className = "results";
        results.innerHTML = `<div class="error-box">${escapeHtml(error.message)}</div>`;
    } finally {
        state.busy = false;
    }
}

function submitSearch(event) {
    event.preventDefault();
    performSearch(queryInput.value);
}

form.addEventListener("submit", submitSearch);

queryInput.addEventListener("input", queueSuggestions);

queryInput.addEventListener("compositionstart", () => {
    state.composing = true;
});

queryInput.addEventListener("compositionend", () => {
    state.composing = false;
    queueSuggestions();
});

queryInput.addEventListener("keydown", (event) => {
    const suggestionsVisible = !suggestions.classList.contains("hidden");
    if (!suggestionsVisible) {
        return;
    }

    if (event.key === "ArrowDown") {
        event.preventDefault();
        setActiveSuggestion(state.activeSuggestion + 1);
    } else if (event.key === "ArrowUp") {
        event.preventDefault();
        setActiveSuggestion(state.activeSuggestion - 1);
    } else if (event.key === "Enter" && state.activeSuggestion >= 0) {
        event.preventDefault();
        applySuggestion(state.activeSuggestion, true);
    } else if (event.key === "Escape") {
        hideSuggestions();
    }
});

queryInput.addEventListener("blur", () => {
    window.setTimeout(hideSuggestions, 120);
});

suggestions.addEventListener("mousedown", (event) => {
    event.preventDefault();
});

suggestions.addEventListener("click", (event) => {
    const button = event.target.closest(".suggestion-item");
    if (!button) {
        return;
    }
    applySuggestion(Number(button.dataset.index), true);
});

clearButton.addEventListener("click", () => {
    queryInput.value = "";
    elapsedTime.textContent = "-";
    setServerState(null, "Idle");
    cancelPendingSuggestions();
    hideSuggestions();
    setEmpty();
    queryInput.focus();
});

setEmpty();
