# RESTful API 调用约定

本文档总结了当前项目中前端与后端之间的 RESTful API 调用约定，供开发与对接参考。

## 一、全局约定

1. **基础 URL (Base URL):** `http://localhost:8080/api`
2. **请求与响应格式:** 数据交互统一使用 JSON 格式。发起带请求体的请求时，必须在 Header 中声明 `Content-Type: application/json`。
3. **身份认证 (Auth):** 标记为“需鉴权”的接口必须在 Header 中携带 JWT Token，格式为：
   `Authorization: Bearer <token>`
4. **统一错误处理:** 当 HTTP 状态码非 2xx（如 400, 401, 403, 404, 500 等）时，后端统一返回包含 `message` 字段的 JSON：
    ```json
    {
        "message": "具体的错误提示信息"
    }
    ```

---

## 二、接口概览 (路由表)

| HTTP 方法 | 路径 | 用途 | 是否幂等 | 请求体 | 需鉴权 |
| :--- | :--- | :--- | :---: | :---: | :---: |
| **POST** | `/login` | 用户登录 | ❌ 否 | 有 | 否 |
| **GET** | `/articles` | 获取文章列表 | ✅ 是 | 无 | 否 |
| **GET** | `/articles/{id}` | 获取单篇文章详情 | ✅ 是 | 无 | 否 |
| **POST** | `/articles` | 创建新文章 | ❌ 否 | 有 | **是** |
| **PUT** | `/articles/{id}` | 全量更新文章 | ✅ 是 | 有 | **是** |
| **PATCH**| `/articles/{id}` | 部分更新文章 | ❌ 否 | 有 | **是** |
| **DELETE**| `/articles/{id}`| 删除文章 | ✅ 是 | 无 | **是** |
| **GET** | `/comments?articleId={id}` | 获取长评/短评列表 | ✅ 是 | 无 | 否 |
| **POST** | `/comments` | 发布文章评论 | ❌ 否 | 有 | **是** |

---

## 三、接口详情

### 1. 认证相关

#### 1.1 用户登录
* **POST** `/login`
* **功能:** 验证用户凭据，并签发访问令牌。
* **请求体:**
    ```json
    { "username": "user123", "password": "password123" }
    ```
* **成功响应 (200 OK):**
    ```json
    { "token": "mock_token_user_1" }
    ```

---

### 2. 文章相关 (Articles)

#### 2.1 获取文章列表
* **GET** `/articles`
* **功能:** 查询系统中所有的文章摘要数据。
* **成功响应 (200 OK):**
    ```json
    [
      { "id": 1, "title": "文章标题A", "author": "UserX", "publishTime": "2026-05-06 10:20:00" },
      { "id": 2, "title": "文章标题B", "author": "UserY", "publishTime": "2026-05-07 11:30:00" }
    ]
    ```

#### 2.2 获取单篇文章详情
* **GET** `/articles/{id}`
* **功能:** 根据指定的 ID 获取文章的详细数据（含 HTML 正文）。
* **成功响应 (200 OK):**
    ```json
    {
      "id": "1",
      "title": "这是一篇测试文章",
      "author": "张三",
      "content": "<p>文章正文...</p>",
      "publishTime": "2026-05-06 10:20:00"
    }
    ```

#### 2.3 创建新文章
* **POST** `/articles`
* **鉴权:** 必须
* **请求体:**
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

#### 2.4 全量更新文章
* **PUT** `/articles/{id}`
* **功能:** 覆盖更新文章的标题和内容（仅限作者本人操作）。
* **鉴权:** 必须
* **请求体:** (同时提供 title 和 content)
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

#### 2.5 部分更新文章
* **PATCH** `/articles/{id}`
* **功能:** 修改文章的特定字段，例如仅修改标题或正文（仅限作者本人操作）。
* **鉴权:** 必须
* **请求体:** (可选提供 title 或 content)
    ```json
    {
      "title": "只修改了这个标题"
    }
    ```
* **成功响应 (200 OK):**
    ```json
    { "message": "Patched" }
    ```

#### 2.6 删除文章
* **DELETE** `/articles/{id}`
* **功能:** 在系统中永久删除一篇指定文章（仅限作者本人操作）。
* **鉴权:** 必须
* **成功响应 (200 OK):**
    ```json
    { "message": "Deleted" }
    ```

---

### 3. 评论相关 (Comments)

#### 3.1 获取文章评论列表
* **GET** `/comments?articleId={id}`
* **功能:** 根据查询参数中的 `articleId` 取得关联评论及其留言者名称。
* **成功响应 (200 OK):**
    ```json
    [
      { "author": "张三", "content": "写的真好！" },
      { "author": "李四", "content": "学习了。" }
    ]
    ```

#### 3.2 发布文章评论
* **POST** `/comments`
* **功能:** 签发者为目标文章追加一条评论评价。
* **鉴权:** 必须
* **请求体:**
    ```json
    {
      "articleId": 1,
      "content": "这是我发布的一条评论"
    }
    ```
* **成功响应 (201 Created):**
    ```json
    {
      "message": "评论成功",
      "articleId": 1
    }
    ```