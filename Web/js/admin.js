document.addEventListener('DOMContentLoaded', () => {
    checkAdminAccess();
    // 后续可以添加更多管理员功能，例如加载用户列表、文章列表等
});

function checkAdminAccess() {
    const userRole = localStorage.getItem('role');
    const username = localStorage.getItem('username');

    if (userRole !== 'admin') {
        // 如果不是管理员，重定向到首页或列表页，并提示无权限
        alert('🚫 您没有权限访问此页面。');
        window.location.href = 'list.html';
        return;
    }

    // 如果是管理员，更新导航区欢迎信息
    const navArea = document.getElementById('navArea');
    if (navArea && username) {
        navArea.innerHTML = `<span>👑 欢迎管理员, ${username}</span> | <a href="list.html">📋 返回列表</a> | <a href="#" onclick="logout()">🚪 退出</a>`;
    }
}

function logout() {
    localStorage.removeItem('token');
    localStorage.removeItem('username');
    localStorage.removeItem('role'); // 退出时清除角色信息
    window.location.href = 'index.html';
}
