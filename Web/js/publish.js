document.addEventListener('DOMContentLoaded', () => {
    // 检查登录状态：如果未登录，提醒并跳转到登录页
    const token = localStorage.getItem('token');
    if (!token) {
        alert('请先登录后再发布文章！');
        window.location.href = 'index.html';
        return;
    }
});

document.getElementById('publishForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    
    const title = document.getElementById('title').value;
    const content = document.getElementById('content').value;
    const publishMessage = document.getElementById('publishMessage');
    
    publishMessage.style.display = 'none';

    try {
        await api.createArticle(title, content);
        
        publishMessage.style.color = 'green';
        publishMessage.innerText = '文章发布成功！正在返回列表...';
        publishMessage.style.display = 'block';
        
        // 发布成功后，延迟返回列表页面
        setTimeout(() => {
            window.location.href = 'list.html';
        }, 1500);
    } catch (error) {
        publishMessage.style.color = 'red';
        publishMessage.innerText = `发布失败: ${error.message}`;
        publishMessage.style.display = 'block';
    }
});