-- Per-member logins with no password: only a token can open them.
CREATE ROLE vault_password_sign_in NOLOGIN;   -- pg_hba lets its members use passwords
CREATE ROLE svt_alice LOGIN;
CREATE ROLE svt_bob LOGIN;
CREATE ROLE svt_carol LOGIN;   -- signs in through the federated "acme" provider
CREATE DATABASE spike;
GRANT CONNECT ON DATABASE spike TO svt_alice, svt_bob, svt_carol;
