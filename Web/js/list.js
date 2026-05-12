document.addEventListener('DOMContentLoaded', () => {
    checkLoginStatus();
    loadArticleList();
});

function checkLoginStatus() {
    const username = localStorage.getItem('username');
    const navArea = document.getElementById('navArea');
    if (username) {
        navArea.innerHTML = `<span>👋 欢迎, ${username}</span> 
            | <a href="publish.html">📝 发布文章</a>
            | <a href="admin.html">⚙️ 后台管理</a> 
            | <a href="#" onclick="logout()">🚪 退出</a>`;
    } else {
        navArea.innerHTML = `<a href="publish.html">📝 发布文章</a> | <a href="index.html">🔑 去登录</a>`;
    }
}

function logout() {
    localStorage.removeItem('token');
    localStorage.removeItem('username');
    window.location.reload();
}

async function loadArticleList() {
    const listContainer = document.getElementById('articleList');
    try {
        const data = await api.getArticles();
        if (data && data.length > 0) {
            listContainer.innerHTML = data.map(article => `
    <div class="article-item">
        <h2><a href="article.html?id=${article.id}">${escapeHtml(article.title)}</a></h2>
        <div class="article-meta">
            <span>✍️ ${article.author || '佚名'}</span>
            <span>📅 ${article.publishTime || '未知'}</span>
        </div>
        <div style="display: flex; justify-content: flex-end; gap: 16px; margin-top: 12px; font-size: 0.85rem; color: #4b5563;">
            <span>❤️ ${article.likes || 0}</span>
            <span>👁️ ${article.views || 0}</span>
        </div>
    </div>
`).join('');
        } else {
            listContainer.innerHTML = '<p>📭 暂无文章，去发布一篇吧～</p>';
        }
    } catch (error) {
        listContainer.innerHTML = `
            <p style="color:#dc2626">⚠️ 无法获取文章列表: ${escapeHtml(error.message)}。</p>
            <div class="article-item">
                <h2><a href="article.html?id=1">📄 示例文章：欢迎来到交流平台</a></h2>
                <div class="article-meta">👤 Admin | 2026-05-08</div>
            </div>
        `;
    }
}

function escapeHtml(str) {
    if (!str) return '';
    return str.replace(/[&<>]/g, function (m) {
        if (m === '&') return '&amp;';
        if (m === '<') return '&lt;';
        if (m === '>') return '&gt;';
        return m;
    });
}