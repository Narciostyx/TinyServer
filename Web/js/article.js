let currentArticleId = null;
let currentUserLiked = false;
let currentArticleAuthor = null;

document.addEventListener('DOMContentLoaded', async () => {
    checkLoginStatus();
    // 先等文章作者信息就绪，再渲染评论：
    // 删除按钮依赖 currentArticleAuthor 判断"文章作者可删他人评论"，若与 loadComments 并行，
    // 评论渲染时 currentArticleAuthor 尚未赋值 → 文章作者名下他人评论会缺失删除按钮
    await loadArticle();
    loadComments();

    document.getElementById('likeArea')?.addEventListener('click', async (e) => {
        const btn = e.target.closest('#likeBtn');
        if (!btn) return;
        const token = localStorage.getItem('accessToken') || localStorage.getItem('token');
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
        const token = localStorage.getItem('accessToken') || localStorage.getItem('token');
        if (!token) {
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
            await api.postComment({ articleId, content });
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
        navArea.innerHTML = `
            <div class="user-info">
                <div class="user-identity">
                    <span class="user-avatar">${username.slice(0, 1).toUpperCase()}</span>
                    <div class="user-text">
                        <div class="user-name">${username}</div>
                        <div class="user-meta">在线</div>
                    </div>
                </div>
                <div class="user-links">
                    <a href="list.html" class="user-link">📋 返回列表</a>
                    <a href="#" class="user-link danger" onclick="logout()">🚪 退出</a>
                </div>
            </div>
        `;
    } else {
        navArea.innerHTML = `
            <div class="user-links">
                <a href="list.html" class="user-link">📋 返回列表</a>
                <a href="index.html" class="user-link">🔑 去登录</a>
            </div>
        `;
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
                <button id="editArticleBtn" class="btn-inline btn-blue">✏️ 编辑文章</button>
                <button id="deleteArticleBtn" class="btn-inline btn-red">🗑️ 删除文章</button>
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
                    ? `<button class="delete-comment-btn btn-inline btn-red comment-del" data-comment-id="${comment.id}">删除</button>`
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

