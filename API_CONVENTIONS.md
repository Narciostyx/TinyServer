# RESTful API 调用约定

本文档总结了当前项目中前端与后端之间的 RESTful API 调用约定，并包含前端请求体/参数格式。

## 一、全局约定

1. **基础 URL (Base URL):** `http://localhost:8080/api`
2. **请求与响应格式:** 数据交互统一使用 JSON。带请求体的请求必须携带 `Content-Type: application/json`。
3. **身份认证 (Auth):** 标记为“需鉴权”的接口必须携带 Access Token：
   `Authorization: Bearer <accessToken>`
4. **统一错误响应:** 当 HTTP 状态码非 2xx 时，返回包含 `message` 字段的 JSON：
    ```json
    { "message": "具体的错误提示信息" }
    ```
5. **前端请求体约定:** 以下示例均为前端发送的 JSON 结构，字段名称区分大小写。
6. **运维接口前缀约定:** 健康检查与指标接口不走 `/api` 前缀，直接使用根路径（如 `/metrics`、`/healthz/live`）。

---

## 二、接口概览 (路由表)

| HTTP 方法 | 路径 | 用途 | 是否幂等 | 请求体 | 需鉴权 |
| :--- | :--- | :--- | :---: | :---: | :---: |
| **POST** | `/register` | 用户注册 | ❌ 否 | 有 | 否 |
| **POST** | `/login` | 用户登录 | ❌ 否 | 有 | 否 |
| **POST** | `/refresh` | 刷新访问令牌 | ❌ 否 | 无 | **是** |
| **GET** | `/articles` | 获取文章列表 | ✅ 是 | 无 | 否 |
| **GET** | `/articles/{id}` | 获取单篇文章详情 | ✅ 是 | 无 | 否（可选） |
| **POST** | `/articles/{id}/like` | 点赞/取消点赞 | ❌ 否 | 无 | **是** |
| **POST** | `/articles/{id}/view` | 上报浏览量 | ❌ 否 | 无 | 否 |
| **POST** | `/articles` | 创建新文章 | ❌ 否 | 有 | **是** |
| **PUT** | `/articles/{id}` | 全量更新文章 | ✅ 是 | 有 | **是** |
| **PATCH** | `/articles/{id}` | 部分更新文章 | ❌ 否 | 有 | **是** |
| **DELETE** | `/articles/{id}` | 删除文章 | ✅ 是 | 无 | **是** |
| **GET** | `/comments?articleId={id}` | 获取评论列表 | ✅ 是 | 无 | 否 |
| **POST** | `/comments` | 发布评论 | ❌ 否 | 有 | **是** |
| **GET** | `/user/stats` | 获取用户统计 | ✅ 是 | 无 | **是** |
| **GET** | `/metrics` | 暴露 Prometheus 指标 | ✅ 是 | 无 | 否 |
| **GET** | `/healthz/live` (`/livez`) | 存活探针（liveness） | ✅ 是 | 无 | 否 |
| **GET** | `/healthz/ready` (`/readyz`) | 就绪探针（readiness） | ✅ 是 | 无 | 否 |

---

## 三、接口详情

### 1. 认证相关

#### 1.0 用户注册
* **POST** `/register`
* **功能:** 注册新用户（普通用户角色，注册后请调用 `/login` 获取令牌）。
* **输入规则:**
  * 用户名：1~8 个字符（中文一字按 1 字符计），仅允许**字母、数字、中文字符**，不含任何特殊字符；
  * 密码：8~12 位，仅允许**字母、数字、中文字符**，且**必须同时包含字母与数字**。
* **前端请求体:**
    ```json
    { "username": "张三", "password": "abc12345" }
    ```
* **成功响应 (201 Created):**
    ```json
    { "message": "注册成功" }
    ```
* **失败响应（非 2xx，均返回 `message`）:**
  * `400` 参数校验失败（长度/字符集/缺少字母数字），`message` 为具体原因；
  * `409` 用户名已存在；
  * `500` 数据库错误。

#### 1.1 用户登录
* **POST** `/login`
* **功能:** 验证用户凭据，并签发访问/刷新令牌。
* **前端请求体:**
    ```json
    { "username": "user123", "password": "password123" }
    ```
* **成功响应 (200 OK):**
    ```json
    {
      "accessToken": "<jwt_access_token>",
      "refreshToken": "<jwt_refresh_token>",
      "role": "admin",
      "expiresIn": 604800
    }
    ```

#### 1.2 刷新访问令牌
* **POST** `/refresh`
* **鉴权:** 必须（携带 refresh token）
* **前端请求体:** 无
* **成功响应 (200 OK):**
    ```json
    {
      "accessToken": "<jwt_access_token>",
      "expiresIn": 604800
    }
    ```

---

### 2. 文章相关 (Articles)

#### 2.1 获取文章列表
* **GET** `/articles`
* **前端请求体:** 无
* **成功响应 (200 OK):**
    ```json
    [
      { "id": 1, "title": "文章标题A", "author": "UserX", "publishTime": "2026-05-06 10:20:00", "likes": 1, "views": 10 },
      { "id": 2, "title": "文章标题B", "author": "UserY", "publishTime": "2026-05-07 11:30:00", "likes": 2, "views": 20 }
    ]
    ```

#### 2.2 获取单篇文章详情
* **GET** `/articles/{id}`
* **鉴权:** 可选（携带 access token 时返回 userLiked）
* **前端请求体:** 无
* **成功响应 (200 OK):**
    ```json
    {
      "id": "1",
      "title": "这是一篇测试文章",
      "author": "张三",
      "content": "<p>文章正文...</p>",
      "publishTime": "2026-05-06 10:20:00",
      "likes": 10,
      "views": 200,
      "userLiked": false
    }
    ```

#### 2.3 点赞/取消点赞
* **POST** `/articles/{id}/like`
* **鉴权:** 必须
* **前端请求体:** 无
* **成功响应 (200 OK):**
    ```json
    { "likes": 12, "liked": true }
    ```

#### 2.4 上报浏览量
* **POST** `/articles/{id}/view`
* **功能:** 用于增加文章浏览量（短时间内重复上报会被去重）。
* **前端请求体:** 无
* **成功响应 (200 OK):**
    ```json
    { "views": 201 }
    ```

#### 2.5 创建新文章
* **POST** `/articles`
* **鉴权:** 必须
* **前端请求体:**
    ```json
    {
      "title": "新文章的标题",
      "content": "<p>新文章的内容</p>"
    }
    ```
* **成功响应 (201 Created):**
    ```json
    { "message": "Create success" }
    ```

#### 2.6 全量更新文章
* **PUT** `/articles/{id}`
* **鉴权:** 必须
* **前端请求体:**
    ```json
    {
      "title": "被完全修改后的标题",
      "content": "<p>被完全修改后的正文内容</p>"
    }
    ```
* **成功响应 (200 OK):**
    ```json
    { "message": "Updated" }
    ```

#### 2.7 部分更新文章
* **PATCH** `/articles/{id}`
* **鉴权:** 必须
* **前端请求体:**
    ```json
    { "title": "只修改了这个标题" }
    ```
* **成功响应 (200 OK):**
    ```json
    { "message": "Patched" }
    ```

#### 2.8 删除文章
* **DELETE** `/articles/{id}`
* **鉴权:** 必须
* **前端请求体:** 无
* **成功响应 (200 OK):**
    ```json
    { "message": "Deleted" }
    ```

---

### 3. 评论相关 (Comments)

#### 3.1 获取文章评论列表
* **GET** `/comments?articleId={id}`
* **前端请求体:** 无
* **成功响应 (200 OK):**
    ```json
    [
      { "id": 1, "author": "张三", "content": "写的真好！" },
      { "id": 2, "author": "李四", "content": "学习了。" }
    ]
    ```

#### 3.2 发布文章评论
* **POST** `/comments`
* **鉴权:** 必须
* **前端请求体:**
    ```json
    {
      "articleId": 1,
      "content": "这是我发布的一条评论"
    }
    ```
* **成功响应 (201 Created):**
    ```json
    { "message": "评论成功", "articleId": 1 }
    ```

---

### 4. 用户相关 (User)

#### 4.1 获取用户统计
* **GET** `/user/stats`
* **鉴权:** 必须
* **前端请求体:** 无
* **成功响应 (200 OK):**
    ```json
    {
      "articleCount": 3,
      "commentCount": 5,
      "totalLikesReceived": 12
    }
    ```

---

### 5. 监控与健康检查 (Ops)

> 说明：本节接口不走 `/api` 前缀，直接访问根路径。

#### 5.1 暴露 Prometheus 指标
* **GET** `/metrics`
* **前端请求体:** 无
* **响应 Content-Type:** `text/plain; version=0.0.4`
* **成功响应 (200 OK):**
  返回 Prometheus 文本格式，包含但不限于：
  - `tinyserver_uptime_seconds`
  - `tinyserver_process_live`
  - `tinyserver_process_ready`
  - `tinyserver_http_requests_total`
  - `tinyserver_http_responses_total`
  - `tinyserver_http_inflight_requests`
  - `tinyserver_http_responses_by_class_total{code_class="2xx|4xx|5xx"}`

#### 5.2 存活探针 (Liveness)
* **GET** `/healthz/live`
* **别名:** `/livez`
* **前端请求体:** 无
* **成功响应 (200 OK):**
    ```json
    { "status": "UP" }
    ```
* **失败响应 (503 Service Unavailable):**
    ```json
    { "status": "DOWN" }
    ```

#### 5.3 就绪探针 (Readiness)
* **GET** `/healthz/ready`
* **别名:** `/readyz`
* **前端请求体:** 无
* **成功响应 (200 OK):**
    ```json
    { "status": "UP" }
    ```
* **失败响应 (503 Service Unavailable):**
    ```json
    { "status": "DOWN" }
    ```
