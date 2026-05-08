document.addEventListener('DOMContentLoaded', () => {
    checkLoginStatus();
    loadArticle();
    loadComments();

    document.getElementById('commentForm').addEventListener('submit', async (e) => {
        e.preventDefault();
        
        const commentMessage = document.getElementById('commentMessage');
        if (!localStorage.getItem('token')) {
            commentMessage.style.color = '#b91c1c';
            commentMessage.innerText = '🔐 请先登录后再发表评论！';
            commentMessage.style.display = 'block';
            setTimeout(() => { window.location.href = 'index.html'; }, 1500);
            return;
        }

        const content = document.getElementById('commentContent').value;
        const urlParams = new URLSearchParams(window.location.search);
        const articleId = urlParams.get('id') || 1; 
        commentMessage.style.display = 'none';

        try {
            await api.postComment(articleId, content);
            document.getElementById('commentContent').value = '';
            commentMessage.style.color = '#15803d';
            commentMessage.innerText = '✅ 评论发布成功！';
            commentMessage.style.display = 'block';
            loadComments();
        } catch (error) {
            commentMessage.style.color = '#b91c1c';
            commentMessage.innerText = `❌ 评论发布失败: ${error.message}`;
            commentMessage.style.display = 'block';
        }
    });
});

function checkLoginStatus() {
    const username = localStorage.getItem('username');
    const navArea = document.getElementById('navArea');
    if (username) {
        navArea.innerHTML = `<span>👋 欢迎, ${username}</span> | <a href="list.html">📋 返回列表</a> | <a href="admin.html">⚙️ 管理</a> | <a href="#" onclick="logout()">🚪 退出</a>`;
    } else {
        navArea.innerHTML = `<a href="list.html">📋 返回列表</a> | <a href="index.html">🔑 去登录</a>`;
    }
}

function logout() {
    localStorage.removeItem('token');
    localStorage.removeItem('username');
    window.location.reload();
}

async function loadArticle() {
    const urlParams = new URLSearchParams(window.location.search);
    const articleId = urlParams.get('id') || 1;

    try {
        const data = await api.getArticleById(articleId);
        document.getElementById('articleTitle').innerText = data.title || '示例文章';
        document.getElementById('articleContent').innerHTML = data.content || '<p>这是一篇示例文章内容。</p>';
    } catch (error) {
        document.getElementById('articleTitle').innerText = '📄 示例文章标题';
        document.getElementById('articleContent').innerHTML = '<p>文章加载失败，请确保后端 localhost:8080 正常运行。</p>';
    }
}

async function loadComments() {
    const urlParams = new URLSearchParams(window.location.search);
    const articleId = urlParams.get('id') || 1;
    const commentsList = document.getElementById('commentsList');
    try {
        const data = await api.getComments(articleId);
        if (data && data.length > 0) {
            commentsList.innerHTML = data.map(comment => `
                <div class="comment">
                    <div class="comment-author">${escapeHtml(comment.author || '匿名')}</div>
                    <div class="comment-text">${escapeHtml(comment.content)}</div>
                </div>
            `).join('');
        } else {
            commentsList.innerHTML = '<p>✨ 暂无评论，快来抢沙发吧！</p>';
        }
    } catch (error) {
        commentsList.innerHTML = '<p>💬 评论加载失败（模拟环境可正常发布到后端）</p>';
    }
}

function escapeHtml(str) {
    if (!str) return '';
    return str.replace(/[&<>]/g, function(m) {
        if (m === '&') return '&amp;';
        if (m === '<') return '&lt;';
        if (m === '>') return '&gt;';
        return m;
    });
}