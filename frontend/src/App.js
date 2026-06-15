import React, { useEffect, useState } from 'react';
import Header from './components/Header';

function App() {
  const [message, setMessage] = useState('로딩 중...');

  useEffect(() => {
    // 백엔드 API 호출
    fetch('http://localhost:5000/api/hello')
      .then((res) => res.json())
      .then((data) => setMessage(data.message))
      .catch((err) => setMessage('백엔드 연결 실패'));
  }, []);

  const containerStyle = {
    textAlign: 'center',
    padding: '5rem 2rem',
    backgroundColor: '#f8f9fa',
    minHeight: '100vh'
  };

  const mainTitleStyle = {
    fontSize: '3rem',
    marginBottom: '1rem',
    color: '#222'
  };

  return (
    <div className="App">
      <Header />
      <div style={containerStyle}>
        <h1 style={mainTitleStyle}>Welcome to My Modern Site</h1>
        <p style={{ fontSize: '1.2rem', color: '#666' }}>{message}</p>
      </div>
    </div>
  );
}

export default App;
