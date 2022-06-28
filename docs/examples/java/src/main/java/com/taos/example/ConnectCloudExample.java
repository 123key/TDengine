package com.taos.example;

// ANCHOR: connect
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.SQLException;
import java.sql.Statement;


public class ConnectCloudExample {
    public static void main(String[] args) throws SQLException {
        String jdbcUrl = System.getenv("TDENGINE_JDBC_URL");
        try(Connection conn = DriverManager.getConnection(jdbcUrl)) {
            try(Statement stmt = conn.createStatement()) {
                // test the connection by firing a query
                stmt.executeQuery("select server_version()");
            }
        }
    }
}
// ANCHOR_END: connect
