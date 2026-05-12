// publish.js
let isSubmitting = false; // 防止重复提交

document.addEventListener('DOMContentLoaded', async () => {
    const token = localStorage.getItem('token');
    if (!token) {
        alert('请先登录后再操作文章！');
        window.location.href = 'index.html';
        return;
    }

    const urlParams = new URLSearchParams(window.location.search);
    const editId = urlParams.get('edit');
    
    if (editId) {
        // 编辑模式：加载现有文章数据
        document.getElementById('pageTitle').innerText = '✏️ 编辑文章';
        try {
            const article = await api.getArticleById(editId);
            document.getElementById('title').value = article.title;
            document.getElementById('content').value = article.content;
            // 存储文章ID，供提交时使用
            window.editArticleId = editId;
        } catch (error) {
            alert('加载文章失败：' + error.message);
            window.location.href = 'list.html';
        }
    } else {
        // 新建模式
        document.getElementById('pageTitle').innerText = '✍️ 发布新文章';
        window.editArticleId = null;
    }

    // 绑定表单提交事件（确保只绑定一次）
    const form = document.getElementById('publishForm');
    // 先移除已有的监听器，避免重复绑定（如果有多个地方调用了该脚本）
    form.removeEventListener('submit', handleSubmit);
    form.addEventListener('submit', handleSubmit);
});

async function handleSubmit(e) {
    e.preventDefault();
    
    // 防止重复提交
    if (isSubmitting) return;
    isSubmitting = true;
    
    const title = document.getElementById('title').value.trim();
    const content = document.getElementById('content').value;
    const publishMessage = document.getElementById('publishMessage');
    const submitBtn = document.querySelector('#publishForm button');
    
    // 禁用按钮，避免二次点击
    submitBtn.disabled = true;
    publishMessage.style.display = 'none';
    
    try {
        if (window.editArticleId) {
            // 编辑模式：调用 PUT 全量更新
            await api.updateArticle(window.editArticleId, title, content);
            publishMessage.innerText = '文章更新成功！正在返回列表...';
        } else {
            // 新建模式：调用 POST 创建
            await api.createArticle(title, content);
            publishMessage.innerText = '文章发布成功！正在返回列表...';
        }
        publishMessage.style.color = 'green';
        publishMessage.style.display = 'block';
        setTimeout(() => {
            window.location.href = 'list.html';
        }, 1500);
    } catch (error) {
        publishMessage.style.color = 'red';
        publishMessage.innerText = `操作失败: ${error.message}`;
        publishMessage.style.display = 'block';
        // 恢复按钮状态
        submitBtn.disabled = false;
        isSubmitting = false;
    }
}