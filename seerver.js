const express = require('express');
const cors = require('cors');
const app = express();
const PORT = 5000;

// 미들웨어 설정
app.use(cors()); // 프론트엔드(다른 포트)에서의 접속을 허용
app.use(express.json());

// 기본 API 루트
app.get('/api/hello', (req, res) => {
  res.json({ message: "백엔드 서버와 성공적으로 연결되었습니다!" });
});

app.listen(PORT, () => {
  console.log(`Server is running on http://localhost:${PORT}`);
});
