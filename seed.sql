-- Seed database for Render PostgreSQL
-- Run this to initialize an existing database

-- Enable UUID extension
CREATE EXTENSION IF NOT EXISTS "uuid-ossp";

-- Users table
CREATE TABLE IF NOT EXISTS users (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    username VARCHAR(100) UNIQUE NOT NULL,
    password_hash VARCHAR(255) NOT NULL,
    first_name VARCHAR(100) NOT NULL,
    last_name VARCHAR(100) NOT NULL,
    role VARCHAR(20) DEFAULT 'user' CHECK (role IN ('user', 'staff', 'admin')),
    is_active BOOLEAN DEFAULT true,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    last_login TIMESTAMP,
    phone VARCHAR(20),
    address TEXT,
    city VARCHAR(100),
    state VARCHAR(50),
    zip_code VARCHAR(20),
    country VARCHAR(100)
);

-- Categories table
CREATE TABLE IF NOT EXISTS categories (
    id SERIAL PRIMARY KEY,
    name VARCHAR(100) UNIQUE NOT NULL,
    description TEXT,
    parent_id INTEGER REFERENCES categories(id),
    is_active BOOLEAN DEFAULT true
);

-- Authors table
CREATE TABLE IF NOT EXISTS authors (
    id SERIAL PRIMARY KEY,
    name VARCHAR(255) NOT NULL,
    bio TEXT,
    birth_date DATE,
    website VARCHAR(255),
    is_active BOOLEAN DEFAULT true
);

-- Books table
CREATE TABLE IF NOT EXISTS books (
    id SERIAL PRIMARY KEY,
    isbn VARCHAR(20) UNIQUE NOT NULL,
    title VARCHAR(500) NOT NULL,
    description TEXT,
    author_id INTEGER REFERENCES authors(id),
    category_id INTEGER REFERENCES categories(id),
    publisher VARCHAR(255),
    publication_date DATE,
    price DECIMAL(10, 2) NOT NULL,
    sale_price DECIMAL(10, 2),
    stock_quantity INTEGER DEFAULT 0,
    low_stock_threshold INTEGER DEFAULT 5,
    isbn_13 VARCHAR(20),
    language VARCHAR(50) DEFAULT 'English',
    pages INTEGER,
    format VARCHAR(50) DEFAULT 'Paperback',
    dimensions VARCHAR(50),
    weight DECIMAL(10, 2),
    rating_average DECIMAL(3, 2) DEFAULT 0,
    review_count INTEGER DEFAULT 0,
    is_featured BOOLEAN DEFAULT false,
    is_bestseller BOOLEAN DEFAULT false,
    is_active BOOLEAN DEFAULT true,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Reviews table
CREATE TABLE IF NOT EXISTS reviews (
    id SERIAL PRIMARY KEY,
    user_id UUID REFERENCES users(id),
    book_id INTEGER REFERENCES books(id),
    rating INTEGER CHECK (rating >= 1 AND rating <= 5),
    comment TEXT,
    is_verified_purchase BOOLEAN DEFAULT false,
    is_approved BOOLEAN DEFAULT true,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Shopping carts table
CREATE TABLE IF NOT EXISTS shopping_carts (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    user_id UUID REFERENCES users(id) UNIQUE,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Cart items table
CREATE TABLE IF NOT EXISTS cart_items (
    id SERIAL PRIMARY KEY,
    cart_id UUID REFERENCES shopping_carts(id),
    book_id INTEGER REFERENCES books(id),
    quantity INTEGER DEFAULT 1 CHECK (quantity > 0),
    added_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Orders table
CREATE TABLE IF NOT EXISTS orders (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    user_id UUID REFERENCES users(id),
    order_number VARCHAR(50) UNIQUE NOT NULL,
    status VARCHAR(50) DEFAULT 'pending' CHECK (status IN ('pending', 'processing', 'shipped', 'delivered', 'cancelled', 'refunded')),
    subtotal DECIMAL(10, 2) NOT NULL,
    tax_amount DECIMAL(10, 2) DEFAULT 0,
    shipping_amount DECIMAL(10, 2) DEFAULT 0,
    discount_amount DECIMAL(10, 2) DEFAULT 0,
    total_amount DECIMAL(10, 2) NOT NULL,
    shipping_address TEXT NOT NULL,
    billing_address TEXT NOT NULL,
    payment_method VARCHAR(50),
    payment_status VARCHAR(50) DEFAULT 'pending' CHECK (payment_status IN ('pending', 'processing', 'completed', 'failed', 'refunded')),
    tracking_number VARCHAR(100),
    notes TEXT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    shipped_at TIMESTAMP,
    delivered_at TIMESTAMP
);

-- Order items table
CREATE TABLE IF NOT EXISTS order_items (
    id SERIAL PRIMARY KEY,
    order_id UUID REFERENCES orders(id),
    book_id INTEGER REFERENCES books(id),
    book_title VARCHAR(500),
    book_isbn VARCHAR(20),
    quantity INTEGER NOT NULL,
    unit_price DECIMAL(10, 2) NOT NULL,
    total_price DECIMAL(10, 2) NOT NULL
);

-- Payment transactions table
CREATE TABLE IF NOT EXISTS payment_transactions (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    order_id UUID REFERENCES orders(id),
    user_id UUID REFERENCES users(id),
    amount DECIMAL(10, 2) NOT NULL,
    currency VARCHAR(3) DEFAULT 'USD',
    payment_method VARCHAR(50),
    status VARCHAR(50) DEFAULT 'pending',
    transaction_id VARCHAR(255),
    gateway_response TEXT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    processed_at TIMESTAMP
);

-- Audit logs table
CREATE TABLE IF NOT EXISTS audit_logs (
    id SERIAL PRIMARY KEY,
    user_id UUID REFERENCES users(id),
    action VARCHAR(100) NOT NULL,
    entity_type VARCHAR(50),
    entity_id VARCHAR(255),
    ip_address INET,
    user_agent TEXT,
    old_values JSONB,
    new_values JSONB,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Webhooks table
CREATE TABLE IF NOT EXISTS webhooks (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    user_id UUID REFERENCES users(id),
    url VARCHAR(500) NOT NULL,
    events JSONB,
    secret VARCHAR(255),
    is_active BOOLEAN DEFAULT true,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    last_triggered TIMESTAMP,
    failure_count INTEGER DEFAULT 0
);

-- Search logs table
CREATE TABLE IF NOT EXISTS search_logs (
    id SERIAL PRIMARY KEY,
    user_id UUID REFERENCES users(id),
    query TEXT NOT NULL,
    filters JSONB,
    results_count INTEGER,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    execution_time_ms INTEGER
);

-- System config table
CREATE TABLE IF NOT EXISTS system_config (
    id SERIAL PRIMARY KEY,
    key VARCHAR(100) UNIQUE NOT NULL,
    value TEXT NOT NULL,
    is_sensitive BOOLEAN DEFAULT false,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Insert default users (only if not exists)
INSERT INTO users (username, password_hash, first_name, last_name, role)
SELECT 'admin', 'password123', 'Admin', 'User', 'admin'
WHERE NOT EXISTS (SELECT 1 FROM users WHERE username = 'admin');

INSERT INTO users (username, password_hash, first_name, last_name, role)
SELECT 'staff', 'password123', 'Staff', 'Member', 'staff'
WHERE NOT EXISTS (SELECT 1 FROM users WHERE username = 'staff');

INSERT INTO users (username, password_hash, first_name, last_name, role)
SELECT 'user', 'password123', 'Regular', 'User', 'user'
WHERE NOT EXISTS (SELECT 1 FROM users WHERE username = 'user');

-- Insert categories (only if empty)
INSERT INTO categories (name, description)
SELECT 'Fiction', 'Fictional literature'
WHERE NOT EXISTS (SELECT 1 FROM categories WHERE name = 'Fiction');

INSERT INTO categories (name, description)
SELECT 'Non-Fiction', 'Non-fictional works'
WHERE NOT EXISTS (SELECT 1 FROM categories WHERE name = 'Non-Fiction');

INSERT INTO categories (name, description)
SELECT 'Science Fiction', 'Science fiction and fantasy'
WHERE NOT EXISTS (SELECT 1 FROM categories WHERE name = 'Science Fiction');

INSERT INTO categories (name, description)
SELECT 'Mystery', 'Mystery and thriller novels'
WHERE NOT EXISTS (SELECT 1 FROM categories WHERE name = 'Mystery');

INSERT INTO categories (name, description)
SELECT 'Biography', 'Biographical works'
WHERE NOT EXISTS (SELECT 1 FROM categories WHERE name = 'Biography');

INSERT INTO categories (name, description)
SELECT 'Technical', 'Technical and programming books'
WHERE NOT EXISTS (SELECT 1 FROM categories WHERE name = 'Technical');

-- Insert authors (only if empty)
INSERT INTO authors (name, bio)
SELECT 'John Smith', 'Bestselling fiction author'
WHERE NOT EXISTS (SELECT 1 FROM authors WHERE name = 'John Smith');

INSERT INTO authors (name, bio)
SELECT 'Jane Doe', 'Technical writer and developer'
WHERE NOT EXISTS (SELECT 1 FROM authors WHERE name = 'Jane Doe');

INSERT INTO authors (name, bio)
SELECT 'Bob Johnson', 'Science fiction novelist'
WHERE NOT EXISTS (SELECT 1 FROM authors WHERE name = 'Bob Johnson');

INSERT INTO authors (name, bio)
SELECT 'Alice Williams', 'Mystery thriller writer'
WHERE NOT EXISTS (SELECT 1 FROM authors WHERE name = 'Alice Williams');

-- Insert books (only if empty)
INSERT INTO books (isbn, title, description, author_id, category_id, price, stock_quantity, is_featured)
SELECT '9780132350884', 'Clean Code', 'A handbook of agile software craftsmanship', 2, 6, 42.99, 25, true
WHERE NOT EXISTS (SELECT 1 FROM books WHERE isbn = '9780132350884');

INSERT INTO books (isbn, title, description, author_id, category_id, price, stock_quantity, is_featured)
SELECT '9780201633610', 'Design Patterns', 'Elements of Reusable Object-Oriented Software', 2, 6, 54.99, 15, true
WHERE NOT EXISTS (SELECT 1 FROM books WHERE isbn = '9780201633610');

INSERT INTO books (isbn, title, description, author_id, category_id, price, stock_quantity, is_featured)
SELECT '9780321125217', 'Domain-Driven Design', 'Tackling Complexity in Heart of Software', 2, 6, 49.99, 20, false
WHERE NOT EXISTS (SELECT 1 FROM books WHERE isbn = '9780321125217');

INSERT INTO books (isbn, title, description, author_id, category_id, price, stock_quantity, is_featured)
SELECT '9780735619678', 'Code Complete', 'A Practical Handbook of Software Construction', 2, 6, 39.99, 30, false
WHERE NOT EXISTS (SELECT 1 FROM books WHERE isbn = '9780735619678');

INSERT INTO books (isbn, title, description, author_id, category_id, price, stock_quantity, is_featured)
SELECT '9780345391803', 'The Hitchhiker''s Guide to the Galaxy', 'A sci-fi comedy classic', 3, 3, 14.99, 50, true
WHERE NOT EXISTS (SELECT 1 FROM books WHERE isbn = '9780345391803');

INSERT INTO books (isbn, title, description, author_id, category_id, price, stock_quantity, is_featured)
SELECT '9780345391802', 'The Restaurant at the End of the Universe', 'Second book in trilogy', 3, 3, 14.99, 45, false
WHERE NOT EXISTS (SELECT 1 FROM books WHERE isbn = '9780345391802');

-- Print results
SELECT 'Users: ' || COUNT(*) as user_count FROM users;
SELECT 'Categories: ' || COUNT(*) as category_count FROM categories;
SELECT 'Authors: ' || COUNT(*) as author_count FROM authors;
SELECT 'Books: ' || COUNT(*) as book_count FROM books;
