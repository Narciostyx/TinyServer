document.addEventListener('DOMContentLoaded', () => {
    checkLoginStatus();
    loadArticleList();
});

async function checkLoginStatus() {
    const username = localStorage.getItem('username');
    const userRole = localStorage.getItem('role');
    const navArea = document.getElementById('navArea');

    if (username) {
        // 用户已登录
        const adminLink = userRole === 'admin' ? ` | <a href="admin.html">⚙️ 后台管理</a>` : '';
        navArea.innerHTML = `
            <div id="userInfo" class="user-info-container">
                <span>👋 欢迎, ${username}</span>
                ${adminLink}
                | <a href="publish.html">📝 发布文章</a>
                | <a href="#" onclick="logout()">🚪 退出</a>
                <div id="userStats" class="user-stats-dropdown">加载中...</div>
            </div>
        `;
        loadUserStats(); // 加载用户统计信息
    } else {
        // 用户未登录
        navArea.innerHTML = `<a href="publish.html">📝 发布文章</a> | <a href="index.html">🔑 去登录</a>`;
    }
}

async function loadUserStats() {
    const statsContainer = document.getElementById('userStats');
    try {
        // 假设 api.js 中有这样一个获取用户统计信息的方法
        const stats = await api.getUserStats(); 
        statsContainer.innerHTML = `
            <ul>
                <li><span class="label">文章发布:</span> <span class="value">${stats.articleCount || 0} 篇</span></li>
                <li><span class="label">收到点赞:</span> <span class="value">${stats.totalLikesReceived || 0} 个</span></li>
                <li><span class="label">发表评论:</span> <span class="value">${stats.commentCount || 0} 条</span></li>
            </ul>
        `;
    } catch (error) {
        statsContainer.innerHTML = `<p style="color:#dc2626; padding: 8px 12px;">无法加载用户信息: ${error.message}</p>`;
    }
}

function logout() {
    localStorage.removeItem('token');
    localStorage.removeItem('username');
    localStorage.removeItem('role');
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