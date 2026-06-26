const state = {
    mode: "web",
    busy: false
};

const form = document.querySelector("#searchForm");
const queryInput = document.querySelector("#queryInput");
const topkInput = document.querySelector("#topkInput");
const langSelect = document.querySelector("#langSelect");
const langOption = document.querySelector("#langOption");
const modeLabel = document.querySelector("#modeLabel");
const resultCount = document.querySelector("#resultCount");
const elapsedTime = document.querySelector("#elapsedTime");
const resultsTitle = document.querySelector("#resultsTitle");
const results = document.querySelector("#results");
const clearButton = document.querySelector("#clearButton");
const serverState = document.querySelector("#serverState");
const modeTabs = Array.from(document.querySelectorAll(".mode-tab"));

function setMode(mode) {
    state.mode = mode;
    modeTabs.forEach((button) => {
        button.classList.toggle("active", button.dataset.mode === mode);
    });

    const isKeyword = mode === "keyword";
    langOption.classList.toggle("hidden", !isKeyword);
    topkInput.value = isKeyword ? "5" : "10";
    modeLabel.textContent = isKeyword ? "关键字推荐" : "网页搜索";
    resultsTitle.textContent = isKeyword ? "推荐词" : "查询结果";
    queryInput.placeholder = isKeyword ? "输入关键词" : "输入查询词";
}

function setServerState(kind, text) {
    serverState.classList.remove("ok", "error");
    if (kind) {
        serverState.classList.add(kind);
    }
    serverState.querySelector("span:last-child").textContent = text;
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

function normalizeTopK() {
    const value = Number.parseInt(topkInput.value, 10);
    if (Number.isNaN(value)) {
        return state.mode === "keyword" ? 5 : 10;
    }
    return Math.max(1, Math.min(50, value));
}

async function callApi(query) {
    const topk = normalizeTopK();
    const endpoint = state.mode === "keyword" ? "/api/suggest" : "/api/search";
    const payload = state.mode === "keyword"
        ? { query, topk, lang: langSelect.value }
        : { query, topk };

    const response = await fetch(endpoint, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload)
    });

    const data = await response.json();
    if (!response.ok || data.error) {
        throw new Error(data.message || data.error || "request failed");
    }
    return data;
}

function renderKeywordResults(items) {
    results.className = "results";
    if (!items.length) {
        setEmpty("没有推荐词");
        return;
    }

    results.innerHTML = `
        <div class="keyword-list">
            ${items.map((item) => `
                <div class="keyword-chip">
                    <span class="chip-word">${escapeHtml(item.word)}</span>
                    <span class="chip-meta">距离 ${escapeHtml(item.distance)}</span>
                    <span class="chip-meta">词频 ${escapeHtml(item.frequency)}</span>
                </div>
            `).join("")}
        </div>
    `;
    resultCount.textContent = String(items.length);
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

async function submitSearch(event) {
    event.preventDefault();
    if (state.busy) {
        return;
    }

    const query = queryInput.value.trim();
    if (!query) {
        setEmpty("请输入查询内容");
        return;
    }

    state.busy = true;
    const start = performance.now();
    setServerState(null, "查询中");
    results.className = "results empty-state";
    results.innerHTML = `
        <div class="empty-mark">◌</div>
        <p>正在查询</p>
    `;

    try {
        const data = await callApi(query);
        const elapsed = Math.round(performance.now() - start);
        elapsedTime.textContent = `${elapsed} ms`;
        setServerState("ok", "后端已连接");

        const items = Array.isArray(data.results) ? data.results : [];
        if (state.mode === "keyword") {
            renderKeywordResults(items);
        } else {
            renderWebResults(items);
        }
    } catch (error) {
        elapsedTime.textContent = "-";
        resultCount.textContent = "0";
        setServerState("error", "服务不可用");
        results.className = "results";
        results.innerHTML = `<div class="error-box">${escapeHtml(error.message)}</div>`;
    } finally {
        state.busy = false;
    }
}

modeTabs.forEach((button) => {
    button.addEventListener("click", () => setMode(button.dataset.mode));
});

form.addEventListener("submit", submitSearch);

clearButton.addEventListener("click", () => {
    queryInput.value = "";
    elapsedTime.textContent = "-";
    setServerState(null, "等待查询");
    setEmpty();
    queryInput.focus();
});

setMode("web");
setEmpty();
