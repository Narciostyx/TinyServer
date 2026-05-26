let currentArticleId = null;
let currentUserLiked = false;
let currentArticleAuthor = null;

document.addEventListener('DOMContentLoaded', () => {
    checkLoginStatus();
    loadArticle();
    loadComments();

    document.getElementById('likeArea')?.addEventListener('click', async (e) => {
    const btn = e.target.closest('#likeBtn');
    if (!btn) return;
    const token = localStorage.getItem('token');
    if (!token) {
        alert('请先登录后点赞');
        return;
    }
    try {
        // 调用点赞接口，期望后端返回 { likes: number, liked: boolean }
        const result = await api.likeArticle(currentArticleId);
        // 更新点赞总数显示
        const likeCountSpan = document.getElementById('likeCount');
        likeCountSpan.innerText = result.likes;
        // 更新当前用户的点赞状态
        currentUserLiked = result.liked;   // 关键：使用后端返回的 liked 字段
        // 刷新按钮样式（爱心颜色/图标）
        updateLikeButtonStyle();
    } catch (error) {
        alert('点赞失败：' + error.message);
    }
});

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

// 绑定点赞按钮事件
document.addEventListener('DOMContentLoaded', () => {
    // ... 原有的评论表单等绑定 ...

    // 点赞按钮的委托事件
    document.getElementById('likeArea')?.addEventListener('click', async (e) => {
        const btn = e.target.closest('#likeBtn');
        if (!btn) return;
        const token = localStorage.getItem('token');
        if (!token) {
            alert('请先登录后点赞');
            return;
        }
        try {
            const result = await api.likeArticle(currentArticleId);
            // 后端返回最新点赞数和用户点赞状态
            document.getElementById('likeCount').innerText = result.likes;
            currentUserLiked = result.liked; // 假设后端返回 liked: true/false
            updateLikeButtonStyle();
        } catch (error) {
            alert('点赞失败：' + error.message);
        }
    });
});

function checkLoginStatus() {
    const username = localStorage.getItem('username');
    const userRole = localStorage.getItem('role');
    const navArea = document.getElementById('navArea');
    if (username) {
        const adminLink = userRole === 'admin' ? ` | <a href="admin.html">⚙️ 管理</a>` : '';
        navArea.innerHTML = `<span>👋 欢迎, ${username}</span> | <a href="list.html">📋 返回列表</a>${adminLink} | <a href="#" onclick="logout()">🚪 退出</a>`;
    } else {
        navArea.innerHTML = `<a href="list.html">📋 返回列表</a> | <a href="index.html">🔑 去登录</a>`;
    }
}

function logout() {
    localStorage.removeItem('token');
    localStorage.removeItem('username');
    localStorage.removeItem('role');
    window.location.reload();
}

async function loadArticle() {
    const urlParams = new URLSearchParams(window.location.search);
    const articleId = urlParams.get('id') || 1;
    currentArticleId = articleId;

    try {
        // 1. 调用浏览量增加接口（不等待结果，避免阻塞页面渲染）
        api.incrementView(articleId).catch(err => console.warn('view increment failed', err));

        // 2. 获取文章详情（包含 likes, views, userLiked）
        const data = await api.getArticleById(articleId);
        document.getElementById('articleTitle').innerText = data.title || '示例文章';
        const formattedContent = data.content ? data.content.replace(/\n/g, '<br>') : '<p>文章内容加载失败。</p>';
        document.getElementById('articleContent').innerHTML = formattedContent;

        // 更新作者信息用于删除/编辑权限（原有逻辑）
        currentArticleAuthor = data.author;

        // 更新点赞数和浏览量显示
        document.getElementById('likeCount').innerText = data.likes || 0;
        document.getElementById('viewCount').innerText = data.views || 0;
        
        // 记录当前用户是否已点赞（后端返回字段 userLiked）
        currentUserLiked = data.userLiked === true;
        updateLikeButtonStyle();

        // 原有编辑/删除按钮逻辑保持不变
        const actionsDiv = document.getElementById('articleActions');
        const currentUser = localStorage.getItem('username');
        if (currentUser && currentArticleAuthor && currentUser === currentArticleAuthor) {
            actionsDiv.innerHTML = `
                <button id="editArticleBtn" style="background: #3b82f6;">✏️ 编辑文章</button>
                <button id="deleteArticleBtn" style="background: #ef4444;">🗑️ 删除文章</button>
            `;
            document.getElementById('editArticleBtn').addEventListener('click', () => {
                window.location.href = `publish.html?edit=${articleId}`;
            });
            document.getElementById('deleteArticleBtn').addEventListener('click', async () => {
                if (confirm('确定删除吗？')) {
                    await api.deleteArticle(articleId);
                    alert('已删除');
                    window.location.href = 'list.html';
                }
            });
        } else {
            actionsDiv.innerHTML = '';
        }
    } catch (error) {
        console.error(error);
        // fallback 处理...
    }
}

function updateLikeButtonStyle() {
    const likeBtn = document.getElementById('likeBtn');
    if (!likeBtn) return;
    const likeCount = document.getElementById('likeCount').innerText;
    if (currentUserLiked) {
        likeBtn.style.color = '#e53e3e';
        likeBtn.style.background = '#fff0f0';
        likeBtn.innerHTML = `❤️ <span id="likeCount">${likeCount}</span>`;
    } else {
        likeBtn.style.color = '#718096';
        likeBtn.style.background = 'transparent';
        likeBtn.innerHTML = `🤍 <span id="likeCount">${likeCount}</span>`;
    }
}

async function loadComments() {
    const urlParams = new URLSearchParams(window.location.search);
    const articleId = urlParams.get('id') || 1;
    const commentsList = document.getElementById('commentsList');
    const currentUser = localStorage.getItem('username'); // 当前登录用户名

    try {
        const data = await api.getComments(articleId);
        if (data && data.length > 0) {
            commentsList.innerHTML = data.map(comment => {
                // 判断是否显示删除按钮：评论作者 或 文章作者
                const canDelete = (currentUser && (currentUser === comment.author || currentUser === currentArticleAuthor));
                const deleteButton = canDelete 
                    ? `<button class="delete-comment-btn" data-comment-id="${comment.id}" style="background:#f87171; padding:4px 12px; font-size:0.8rem; width:auto; margin-left:12px;">删除</button>`
                    : '';
                const formattedComment = escapeHtml(comment.content).replace(/\n/g, '<br>');
                return `
                    <div class="comment" data-comment-id="${comment.id}">
                        <div class="comment-author">${escapeHtml(comment.author || '匿名')}</div>
                        <div class="comment-text">${formattedComment}</div>
                        <div style="display:flex; justify-content:flex-end; margin-top:8px;">${deleteButton}</div>
                    </div>
                `;
            }).join('');

            // 绑定删除按钮事件
            document.querySelectorAll('.delete-comment-btn').forEach(btn => {
                btn.addEventListener('click', async (e) => {
                    e.stopPropagation();
                    const commentId = btn.getAttribute('data-comment-id');
                    if (confirm('确定要删除这条评论吗？')) {
                        try {
                            await api.deleteComment(commentId);
                            // 删除成功后刷新评论列表
                            loadComments();
                        } catch (error) {
                            alert(`删除失败: ${error.message}`);
                        }
                    }
                });
            });
        } else {
            commentsList.innerHTML = '<p>✨ 暂无评论，快来抢沙发吧！</p>';
        }
    } catch (error) {
        commentsList.innerHTML = '<p>💬 评论加载失败</p>';
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

