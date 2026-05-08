/**
 * 当 DOM 加载完成时初始化页面：
 * 1. 检查用户的登录状态并更新导航区。
 * 2. 尝试加载文章正文。
 * 3. 尝试加载现有评论。
 */
document.addEventListener('DOMContentLoaded', () => {
    checkLoginStatus();
    loadArticle();
    loadComments();

    /**
     * 监听评论表单的提交事件
     */
    document.getElementById('commentForm').addEventListener('submit', async (e) => {
        e.preventDefault();
        
        const commentMessage = document.getElementById('commentMessage');
        if (!localStorage.getItem('token')) {
            commentMessage.style.color = 'red';
            commentMessage.innerText = '请先登录后再发表评论！';
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
            commentMessage.style.color = 'green';
            commentMessage.innerText = '评论发布成功！';
            commentMessage.style.display = 'block';
            loadComments();
        } catch (error) {
            commentMessage.style.color = 'red';
            commentMessage.innerText = `评论发布失败: ${error.message}`;
            commentMessage.style.display = 'block';
        }
    });
});

/**
 * 检查本地缓存中的用户登录信息，
 * 动态显示登录状态（允许游客查看，不强行重定向）。
 */
function checkLoginStatus() {
    const username = localStorage.getItem('username');
    const navArea = document.getElementById('navArea');
    if (username) {
        navArea.innerHTML = `<span>欢迎, ${username}</span> | <a href="list.html">返回列表</a> | <a href="#" onclick="logout()">退出</a>`;
    } else {
        navArea.innerHTML = `<a href="list.html">返回列表</a> | <a href="index.html">去登录</a>以便发表评论`;
    }
}

/**
 * 用户退出操作，清除本地缓存的用户信息并刷新当前页面。
 */
function logout() {
    localStorage.removeItem('token');
    localStorage.removeItem('username');
    window.location.reload();
}

/**
 * 异步获取文章内容并在页面上渲染。
 */
async function loadArticle() {
    // 从 URL 参数中解析出 id (如 article.html?id=2)
    const urlParams = new URLSearchParams(window.location.search);
    const articleId = urlParams.get('id') || 1; // 默认回退为1

    try {
        const data = await api.getArticleById(articleId);
        document.getElementById('articleTitle').innerText = data.title || '示例文章标题';
        document.getElementById('articleContent').innerHTML = data.content || '<p>这是一篇示例文章内容。</p>';
    } catch (error) {
        document.getElementById('articleTitle').innerText = '示例文章标题';
        document.getElementById('articleContent').innerHTML = '<p>文章加载失败或这是模拟的前端页面。请确保后端 localhost:8080 正常运行并在 /api/article/1 返回数据。</p>';
    }
}

/**
 * 异步获取文章评论并在页面上生成评论列表。
 */
async function loadComments() {
    const urlParams = new URLSearchParams(window.location.search);
    const articleId = urlParams.get('id') || 1;
    const commentsList = document.getElementById('commentsList');
    try {
        const data = await api.getComments(articleId);
        if (data && data.length > 0) {
            commentsList.innerHTML = data.map(comment => `
                <div class="comment">
                    <div class="comment-author">${comment.author || '匿名'}</div>
                    <div class="comment-text">${comment.content}</div>
                </div>
            `).join('');
        } else {
            commentsList.innerHTML = '<p>暂无评论，快来抢沙发吧！</p>';
        }
    } catch (error) {
        commentsList.innerHTML = '<p>评论加载失败或者是模拟环境运行。尝试发布评论将被发送到 localhost:8080。</p>';
    }
}