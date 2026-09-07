/**
 * 基础 API 请求 URL:
 * 全局约定: 基础 URL 为 http://127.0.0.1:8080/api (规避部分机器上 localhost 的 IPv6 解析问题)
 */
const API_BASE_URL = 'http://127.0.0.1:8080/api';

/**
 * 封装的 fetch 请求函数，处理通用的头部信息、身份验证和错误处理
 * 严格遵循 API_CONVENTIONS.md 中的统一规范
 * 
 * @param {string} endpoint - API 相对路径
 * @param {Object} options - fetch 选项配置
 * @param {string} authType - 认证类型: 'none' | 'access' | 'refresh' | 'optional'
 * @returns {Promise<any>} 返回解析后的 JSON 数据
 */
async function apiFetch(endpoint, options = {}, authType = 'none') {
    // 1. 全局约定: 数据交互统一使用 JSON 格式，有 body 则添加 Content-Type
    if (options.body && typeof options.body === 'string') {
        options.headers = {
            ...options.headers,
            'Content-Type': 'application/json'
        };
    }

    // 2. 身份认证 (Auth): 仅在约定要求鉴权的接口上携带 JWT Token
    if (authType !== 'none') {
        const token = authType === 'refresh'
            ? localStorage.getItem('refreshToken')
            : (localStorage.getItem('accessToken') || localStorage.getItem('token'));
        if (!token && authType !== 'optional') {
            throw new Error('缺少授权令牌，请先登录');
        }
        if (token) {
            options.headers = {
                ...options.headers,
                'Authorization': `Bearer ${token}`
            };
        }
    }

    try {
        const response = await fetch(`${API_BASE_URL}${endpoint}`, options);

        // 3. 统一错误处理: 当 HTTP 状态码非 2xx 时
        if (!response.ok) {
            let errorMessage = `HTTP Error ${response.status}`;
            try {
                const errorData = await response.json();
                errorMessage = errorData.message || errorMessage;
            } catch (e) {
                // Ignore fallback to default error message
            }
            throw new Error(errorMessage);
        }

        // 应对 204 No Content 或空响应体避免解析导致的报错
        const text = await response.text();
        return text ? JSON.parse(text) : {};
    } catch (error) {
        console.error(`API Fetch Error (${endpoint}):`, error);
        throw error;
    }
}

/**
 * 依照 API_CONVENTIONS.md 提供具体化接口调用代理对象
 */
const api = {
    // 1. 认证相关
    register: ({ username, password }) =>
        apiFetch('/register', { method: 'POST', body: JSON.stringify({ username, password }) }),

    login: ({ username, password }) =>
        apiFetch('/login', { method: 'POST', body: JSON.stringify({ username, password }) }),

    refreshAccessToken: () =>
        apiFetch('/refresh', { method: 'POST' }, 'refresh'),

    getUserStats: () =>
        apiFetch('/user/stats', { method: 'GET' }, 'access'),

    // 2. 文章相关
    getArticles: () =>
        apiFetch('/articles', { method: 'GET' }),

    getArticleById: (id) =>
        apiFetch(`/articles/${id}`, { method: 'GET' }, 'optional'),

    // 点赞/取消点赞文章（toggle）
    likeArticle: (id) =>
        apiFetch(`/articles/${id}/like`, { method: 'POST' }, 'access'),

    // 增加文章浏览量
    incrementView: (id) =>
        apiFetch(`/articles/${id}/view`, { method: 'POST' }),

    createArticle: ({ title, content }) =>
        apiFetch('/articles', { method: 'POST', body: JSON.stringify({ title, content }) }, 'access'),

    updateArticle: (id, { title, content }) =>
        apiFetch(`/articles/${id}`, { method: 'PUT', body: JSON.stringify({ title, content }) }, 'access'),

    patchArticle: (id, data) =>
        apiFetch(`/articles/${id}`, { method: 'PATCH', body: JSON.stringify(data) }, 'access'),

    deleteArticle: (id) =>
        apiFetch(`/articles/${id}`, { method: 'DELETE' }, 'access'),

    // 3. 评论相关
    getComments: (articleId) =>
        apiFetch(`/comments?articleId=${articleId}`, { method: 'GET' }),

    postComment: ({ articleId, content }) =>
        apiFetch('/comments', { method: 'POST', body: JSON.stringify({ articleId, content }) }, 'access'),

    deleteComment: (commentId) =>
        apiFetch(`/comments/${commentId}`, { method: 'DELETE' }, 'access')
};
