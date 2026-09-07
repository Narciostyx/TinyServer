/**
 * 注册页逻辑：
 * 1. 登录/注册面板切换；
 * 2. 注册前端校验（与后端 /api/register 规则一致：用户名 1~8 字符、密码 8~12 位含字母数字、
 *    两者均仅允许字母/数字/中文，不允许特殊字符）；
 * 3. 成功后返回登录面板并预填用户名。
 */

// ---------- 面板切换 ----------
const loginPanel = document.getElementById('loginPanel');
const registerPanel = document.getElementById('registerPanel');
const authTitle = document.getElementById('authTitle');
const loginMessage = document.getElementById('loginMessage');
const registerMessage = document.getElementById('registerMessage');

function showLogin() {
    loginPanel.style.display = 'block';
    registerPanel.style.display = 'none';
    authTitle.innerText = '🔐 用户登录';
    registerMessage.style.display = 'none';
}

function showRegister() {
    loginPanel.style.display = 'none';
    registerPanel.style.display = 'block';
    authTitle.innerText = '📝 用户注册';
    loginMessage.style.display = 'none';
}

document.getElementById('showRegisterLink').addEventListener('click', (e) => {
    e.preventDefault();
    showRegister();
});

document.getElementById('backToLoginLink').addEventListener('click', (e) => {
    e.preventDefault();
    showLogin();
});

// ---------- 注册校验 ----------
function showRegisterError(msg) {
    registerMessage.style.color = 'red';
    registerMessage.innerText = msg;
    registerMessage.style.display = 'block';
}

// 按码点展开字符串（中文字符一字算一个字符）
function codePoints(str) {
    return Array.from(str); // ES6 Array.from 按 code point 迭代
}

function isAllowedCharCode(c) {
    const isLetter = (c >= 0x61 && c <= 0x7a) || (c >= 0x41 && c <= 0x5a); // a-z A-Z
    const isDigit = c >= 0x30 && c <= 0x39;                                 // 0-9
    const isCjk = (c >= 0x4e00 && c <= 0x9fff) || (c >= 0x3400 && c <= 0x4dbf); // CJK 基本区 + 扩展A
    return isLetter || isDigit || isCjk;
}

function validateRegister(username, password, confirm) {
    const userChars = codePoints(username);
    const pwdChars = codePoints(password);

    // 用户名
    if (userChars.length < 1 || userChars.length > 8) {
        return '用户名长度需为 1~8 个字符';
    }
    if (!userChars.every((ch) => isAllowedCharCode(ch.codePointAt(0)))) {
        return '用户名只能包含字母、数字或中文字符';
    }

    // 密码
    if (pwdChars.length < 8 || pwdChars.length > 12) {
        return '密码长度需为 8~12 位';
    }
    if (!pwdChars.every((ch) => isAllowedCharCode(ch.codePointAt(0)))) {
        return '密码只能包含字母、数字或中文字符';
    }
    const hasLetter = pwdChars.some((ch) => {
        const c = ch.codePointAt(0);
        return (c >= 0x61 && c <= 0x7a) || (c >= 0x41 && c <= 0x5a);
    });
    const hasDigit = pwdChars.some((ch) => {
        const c = ch.codePointAt(0);
        return c >= 0x30 && c <= 0x39;
    });
    if (!hasLetter || !hasDigit) {
        return '密码必须同时包含字母和数字';
    }

    // 确认密码
    if (password !== confirm) {
        return '两次输入的密码不一致';
    }
    return null;
}

// ---------- 注册提交 ----------
document.getElementById('registerForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    registerMessage.style.display = 'none';

    const username = document.getElementById('regUsername').value.trim();
    const password = document.getElementById('regPassword').value;
    const confirm = document.getElementById('regPasswordConfirm').value;

    const err = validateRegister(username, password, confirm);
    if (err) {
        showRegisterError(err);
        return;
    }

    try {
        await api.register({ username, password });
        registerMessage.style.color = 'green';
        registerMessage.innerText = '注册成功，正在跳转登录...';
        registerMessage.style.display = 'block';

        // 1.2s 后回到登录面板并预填用户名
        setTimeout(() => {
            document.getElementById('username').value = username;
            document.getElementById('password').value = '';
            showLogin();
        }, 1200);
    } catch (error) {
        showRegisterError(`注册失败: ${error.message}`);
    }
});
