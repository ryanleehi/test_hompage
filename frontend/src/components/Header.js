import React from 'react';

const Header = () => {
  const navItems = ['Home', 'About', 'Services', 'Contact'];

  const headerStyle = {
    display: 'flex',
    justifyContent: 'space-between',
    alignItems: 'center',
    padding: '1rem 2rem',
    backgroundColor: '#ffffff',
    boxShadow: '0 2px 10px rgba(0,0,0,0.1)',
    position: 'sticky',
    top: 0,
    zIndex: 1000
  };

  const logoStyle = {
    fontSize: '1.5rem',
    fontWeight: 'bold',
    color: '#333',
    cursor: 'pointer'
  };

  const navStyle = {
    display: 'flex',
    gap: '2rem',
    listStyle: 'none'
  };

  const linkStyle = {
    textDecoration: 'none',
    color: '#555',
    fontWeight: '500',
    fontSize: '1rem',
    transition: 'color 0.3s'
  };

  return (
    <header style={headerStyle}>
      <div style={logoStyle}>MyBrand</div>
      <nav>
        <ul style={navStyle}>
          {navItems.map((item) => (
            <li key={item}>
              <a href={`#${item.toLowerCase()}`} style={linkStyle} 
                 onMouseOver={(e) => e.target.style.color = '#007bff'}
                 onMouseOut={(e) => e.target.style.color = '#555'}>
                {item}
              </a>
            </li>
          ))}
        </ul>
      </nav>
    </header>
  );
};

export default Header;
