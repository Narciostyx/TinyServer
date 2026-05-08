document.addEventListener('DOMContentLoaded', () => {
    checkLoginStatus();
    loadArticleList();
});

function checkLoginStatus() {
    const username = localStorage.getItem('username');
    const navArea = document.getElementById('navArea');
    if (username) {
        navArea.innerHTML = `<span>欢迎, ${username}</span> 
            | <a href="publish.html">发布文章</a>
            | <a href="admin.html">后台管理</a> 
            | <a href="#" onclick="logout()">退出</a>`;
    } else {
        navArea.innerHTML = `<a href="publish.html">发布文章</a> | <a href="index.html">去登录</a>`;
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
                    <h2><a href="article.html?id=${article.id}">${article.id}. ${article.title}</a></h2>
                    <div class="article-meta">发布人: ${article.author || '佚名'} | 发布时间: ${article.publishTime || '未知'}</div>
                </div>
            `).join('');
        } else {
            listContainer.innerHTML = '<p>暂无文章发布。</p>';
        }
    } catch (error) {
        // Mock fallback 如果后端还没实现此接口
        listContainer.innerHTML = `
            <p style="color:red">无法获取文章列表: ${error.message}。</p>
            <p>请检查网络连接或后端服务是否正常运行。</p>
            <div class="article-item">
                <h2><a href="article.html?id=1">Hello, This is a Sample Article.</a></h2>
                <div class="article-meta">发布人: Admin | 发布时间: 2026-05-07 10:00:00</div>
            </div>
        `;
    }
}