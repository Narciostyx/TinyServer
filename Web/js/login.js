/**
 * 监听登录表单提交事件，阻止默认行为，并以异步方式向上游接口提交用户名和密码进行验证。
 * 成功登录后将令牌（token）保存到本地并跳转到文章页。
 */
document.getElementById('loginForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    
    // 获取用户输入的账户信息
    const username = document.getElementById('username').value;
    const password = document.getElementById('password').value;
    const loginMessage = document.getElementById('loginMessage');
    loginMessage.style.display = 'none';

    try {
        const data = await api.login(username, password);
        
        if (data.token) {
            localStorage.setItem('token', data.token);
            localStorage.setItem('username', username);
            loginMessage.style.color = 'green';
            loginMessage.innerText = '登录成功，正在跳转...';
            loginMessage.style.display = 'block';
            setTimeout(() => {
                window.location.href = 'list.html';
            }, 500);
        } else {
            loginMessage.style.color = 'red';
            loginMessage.innerText = '登录失败：未能获取Token。';
            loginMessage.style.display = 'block';
        }
    } catch (error) {
        loginMessage.style.color = 'red';
        loginMessage.innerText = `登录出错: ${error.message}`;
        loginMessage.style.display = 'block';
    }
});