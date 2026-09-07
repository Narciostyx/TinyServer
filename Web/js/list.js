document.addEventListener('DOMContentLoaded', () => {
    checkLoginStatus();
    loadArticleList();
});

async function checkLoginStatus() {
    const username = localStorage.getItem('username');
    const navArea = document.getElementById('navArea');

    if (username) {
        // 用户已登录
        navArea.innerHTML = `
            <div id="userInfo" class="user-info">
                <div class="user-identity">
                    <span class="user-avatar">${username.slice(0, 1).toUpperCase()}</span>
                    <div class="user-text">
                        <div class="user-name">${username}</div>
                        <div class="user-meta">在线</div>
                    </div>
                </div>
                <div class="user-links">
                    <a href="publish.html" class="user-link">📝 发布文章</a>
                    <a href="#" class="user-link danger" onclick="logout()">🚪 退出</a>
                </div>
                <div id="userStats" class="user-stats-dropdown">加载中...</div>
            </div>
        `;
        loadUserStats(); // 加载用户统计信息
    } else {
        // 用户未登录
        navArea.innerHTML = `
            <div class="user-links">
                <a href="publish.html" class="user-link">📝 发布文章</a>
                <a href="index.html" class="user-link">🔑 去登录</a>
            </div>
        `;
    }
}

async function loadUserStats() {
    const statsContainer = document.getElementById('userStats');
    try {
        // 假设 api.js 中有这样一个获取用户统计信息的方法
        const stats = await api.getUserStats(); 
        statsContainer.innerHTML = `
            <div class="user-stats-grid">
                <div class="stat-item">
                    <div class="stat-label">文章发布</div>
                    <div class="stat-value">${stats.articleCount || 0}</div>
                </div>
                <div class="stat-item">
                    <div class="stat-label">收到点赞</div>
                    <div class="stat-value">${stats.totalLikesReceived || 0}</div>
                </div>
                <div class="stat-item">
                    <div class="stat-label">发表评论</div>
                    <div class="stat-value">${stats.commentCount || 0}</div>
                </div>
            </div>
        `;
    } catch (error) {
        statsContainer.innerHTML = `<p style="color:#dc2626; padding: 8px 12px;">无法加载用户信息: ${error.message}</p>`;
    }
}

function logout() {
    localStorage.removeItem('token');
    localStorage.removeItem('accessToken');
    localStorage.removeItem('refreshToken');
    localStorage.removeItem('tokenExpiresIn');
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
            <span class="who">
                <span class="mini-avatar">${escapeHtml((article.author || '佚').slice(0, 1))}</span>
                ${escapeHtml(article.author || '佚名')}
            </span>
            <span class="dot"></span>
            <span>📅 ${escapeHtml(article.publishTime || '未知')}</span>
        </div>
        <div class="article-footer">
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